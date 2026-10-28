#include "cpu_scheduler.h"
#include "priority_ceiling.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/power.h>
#include <pthread.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace cpu_scheduler {
namespace {
struct Entry {
    int id = -1; Role role = Role::Parser; int core = -1; int64_t moved = 0;
    // Performance-log interval: one-second samples on each core, core changes, the kernel thread name.
    std::array<uint16_t, 4> seconds{}; uint16_t moves = 0; char name[32] = {};
};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
std::array<Entry, 16> threads;
int base_priority = 0x10000100;
int audio_active = 0, video_active = 0;
// Startup flood: the Parser gets a core of its own (CPU3 when usable), the other background
// threads keep off it. Disabled while video owns cores.
bool parser_pinned = false;
enum ProbeState { Checking = -1, Timeout = -2, CreateFailed = -3,
    StartFailed = -4, AffinityFailed = -5, WrongCore = -6, Confirmed = 1 };
std::atomic<int> fourth{Checking};
std::atomic<int> probe_error{0}, observed_core{-1};
int probe_id = -1;
int64_t probe_started = 0;
int probe_attempts = 0;
int probe_info_error = 0;
std::array<int, 4> load{{-1,-1,-1,-1}};
std::array<uint64_t, 4> previous_idle{};
int64_t last_tick = 0, last_sample = 0;
// Performance-log interval, reset by writeReport(): per-core busy percent and the ARM clock.
std::array<uint32_t, 4> busy_sum{}; std::array<int, 4> busy_max{{-1,-1,-1,-1}};
uint32_t report_samples = 0; int clock_min = 0, clock_max = 0;
const int masks[] = {SCE_KERNEL_CPU_MASK_USER_0, SCE_KERNEL_CPU_MASK_USER_1,
    SCE_KERNEL_CPU_MASK_USER_2, SCE_KERNEL_CPU_MASK_SYSTEM};
void check(const char* what, int rc) {
    if (rc < 0) std::fprintf(stderr, "CPU scheduler %s: 0x%08X\n", what, rc);
}
int probe(SceSize, void*) {
    // A plugin may accept the affinity request without actually scheduling CPU3.
    int core = sceKernelGetCpuId();
    observed_core = core;
    // A late execution is still valid proof after the UI timeout.
    fourth = core == 3 ? Confirmed : WrongCore;
    return 0;
}
void start_probe() { // Called on UI only; never overlap probe threads.
    if (probe_id >= 0) return;
    ++probe_attempts;
    probe_started = sceKernelGetSystemTimeWide();
    probe_error = 0; observed_core = -1; probe_info_error = 0;
    fourth = Checking;
    // Use the documented user-thread creation priority, not an unchecked
    // offset from the main thread's reported runtime priority.
    constexpr int probe_priority = 0x10000100;
    probe_id = sceKernelCreateThread("vitagram_cpu3_probe", probe, probe_priority, 16384, 0, masks[3], nullptr);
    if (probe_id < 0) {
        probe_error = probe_id; fourth = CreateFailed;
        check("CPU3 create", probe_id); return;
    }
    int rc = sceKernelStartThread(probe_id, 0, nullptr);
    if (rc < 0) {
        probe_error = rc; fourth = StartFailed; check("CPU3 start", rc);
        int deleted = sceKernelDeleteThread(probe_id);
        if (deleted >= 0) probe_id = -1;
        else probe_info_error = deleted;
    }
}
// Prefetch (read-ahead and decryption for playback) is placed like the other background work:
// on the least loaded core that audio and the video decoder do not own.
bool background(Role role) { return role == Role::Network || role == Role::Parser || role == Role::Storage || role == Role::Prefetch; }
void move(Entry& thread, int core, int64_t now) {
    if (thread.core == core) return;
    int rc = sceKernelChangeThreadCpuAffinityMask(thread.id, masks[core]);
    if (rc >= 0) { if (thread.core >= 0) ++thread.moves; thread.core = core; thread.moved = now; }
    else {
        check("affinity", rc);
        if (core == 3) { probe_error = rc; fourth = AffinityFailed; }
        // Keep a background worker off the decoder core during playback,
        // including when the preferred CPU3 is unavailable.
        int fallback = video_active ? 0 : 2;
        if (core != fallback && sceKernelChangeThreadCpuAffinityMask(thread.id, masks[fallback]) >= 0) {
            if (thread.core >= 0) ++thread.moves;
            thread.core = fallback; thread.moved = now;
        }
    }
}
void balance(bool urgent) { // mutex held
    int64_t now = sceKernelGetSystemTimeWide();
    int assigned[4] = {};
    const int parser_core = parser_pinned && !video_active ? (fourth == Confirmed ? 3 : 2) : -1;
    for (auto& thread : threads) {
        if (thread.id < 0 || !background(thread.role)) continue;
        if (thread.role == Role::Parser && parser_core >= 0) {
            move(thread, parser_core, now);
            if (thread.core >= 0) ++assigned[thread.core];
            continue;
        }
        int best = video_active ? 0 : 2, best_score = 10000;
        bool found = false;
        auto score = [&](int core) {
            return (load[core] < 0 ? 40 : load[core]) + assigned[core] * 30 +
                (core == 0 ? 30 : 0);
        };
        for (int core = 0; core <= 3; ++core) {
            if ((core == 0 && !video_active) || (core == 1 && audio_active) ||
                (core == 2 && video_active) || (core == 3 && fourth != Confirmed) || core == parser_core) continue;
            if (score(core) < best_score) { best = core; best_score = score(core); found = true; }
        }
        // No other core is free (audio holds CPU1 and CPU3 is unusable): share rather than stall.
        if (!found && parser_core >= 0) { if (thread.core >= 0) ++assigned[thread.core]; continue; }
        bool forbidden = thread.core < 0 || (thread.core == 0 && !video_active) ||
            (thread.core == 1 && audio_active) || (thread.core == 2 && video_active) ||
            (thread.core == 3 && fourth != Confirmed) || thread.core == parser_core;
        // Avoid chasing every transient spike and repeatedly evicting warm caches.
        if (urgent || forbidden || (now - thread.moved >= 5000000 && best_score + 20 < score(thread.core)))
            move(thread, best, now);
        if (thread.core >= 0) ++assigned[thread.core];
    }
}
}
void init() {
    SceKernelThreadInfo info{}; info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0) base_priority = info.currentPriority;
    // Lock holders the UI waits on run at the UI thread priority (offset 0 below).
    priority_ceiling::ui_priority = std::max(32, base_priority);
    start_probe();
}
Thread::Thread(Role role) : id_(sceKernelGetThreadId()) {
    pthread_mutex_lock(&mutex);
    // Prefetch runs just behind the decoder it feeds, ahead of the other background work.
    int offset = role == Role::Audio ? -8 : role == Role::Ui ? 0 : role == Role::Video ? 12 :
        role == Role::Prefetch ? 16 : role == Role::Storage ? 28 : 24;
    check("priority", sceKernelChangeThreadPriority(id_, std::max(32, base_priority + offset)));
    for (auto& thread : threads) if (thread.id < 0) {
        thread = Entry{};
        thread.id = id_; thread.role = role;
        SceKernelThreadInfo info{}; info.size = sizeof(info);
        if (sceKernelGetThreadInfo(id_, &info) >= 0) std::strncpy(thread.name, info.name, sizeof(thread.name) - 1);
        int initial_core = role == Role::Ui ? 0 : role == Role::Audio ? 1 :
            (background(role) && video_active ? 0 : 2);
        move(thread, initial_core, sceKernelGetSystemTimeWide());
        break;
    }
    balance(true);
    pthread_mutex_unlock(&mutex);
}
Thread::~Thread() {
    pthread_mutex_lock(&mutex);
    for (auto& thread : threads) if (thread.id == id_) thread = Entry{};
    pthread_mutex_unlock(&mutex);
}
Media::Media(Role role) : role_(role) {
    pthread_mutex_lock(&mutex);
    if (role == Role::Audio) ++audio_active; else ++video_active;
    // Evict background work before the first audio block is submitted.
    balance(true);
    pthread_mutex_unlock(&mutex);
}
Media::~Media() {
    pthread_mutex_lock(&mutex);
    if (role_ == Role::Audio) --audio_active; else --video_active;
    // Restore capacity immediately when capture/playback releases a core.
    balance(true);
    pthread_mutex_unlock(&mutex);
}
void tick() {
    int64_t now = sceKernelGetSystemTimeWide();
    if (now - last_tick < 1000000) return;
    last_tick = now;
    if (now - probe_started > 5000000) {
        int pending = Checking;
        fourth.compare_exchange_strong(pending, Timeout); // Still no execution proof.
    }
    if (probe_id >= 0) {
        SceKernelThreadInfo info{}; info.size = sizeof(info);
        int rc = sceKernelGetThreadInfo(probe_id, &info);
        if (rc < 0) probe_info_error = rc;
        else if (info.status == SCE_THREAD_DORMANT) {
            rc = sceKernelDeleteThread(probe_id);
            if (rc >= 0) { probe_id = -1; probe_info_error = 0; }
            else probe_info_error = rc;
        }
    }
    // At most three attempts, 30 seconds apart. A timed-out but live thread
    // must finish before another can be created; never terminate it forcibly.
    if (fourth != Confirmed && probe_id < 0 && probe_attempts < 3 && now-probe_started >= 30000000)
        start_probe();
    SceKernelSystemInfo info{}; info.size = sizeof(info);
    int rc = sceKernelGetSystemInfo(&info);
    const int clock = scePowerGetArmClockFrequency();
    pthread_mutex_lock(&mutex);
    bool sampled = false;
    if (rc >= 0) {
        for (int core = 0; core < 4; ++core) {
            uint64_t idle = info.cpuInfo[core].idleClock;
            if (last_sample && idle >= previous_idle[core] && now > last_sample) {
                uint64_t delta = idle - previous_idle[core];
                int busy = 100 - (int)std::min<uint64_t>(100, delta * 100 / (now-last_sample));
                load[core] = load[core] < 0 ? busy : (load[core] + busy) / 2;
                busy_sum[core] += busy; busy_max[core] = std::max(busy_max[core], busy);
                sampled = true;
            }
            previous_idle[core] = idle;
        }
        last_sample = now;
    } else { load.fill(-1); last_sample = 0; }
    balance(false);
    if (sampled) {
        ++report_samples;
        if (clock > 0) { clock_min = clock_min ? std::min(clock_min, clock) : clock; clock_max = std::max(clock_max, clock); }
    }
    // Where each thread ran during this second, after balancing.
    for (auto& thread : threads)
        if (thread.id >= 0 && thread.core >= 0 && thread.seconds[thread.core] < UINT16_MAX) ++thread.seconds[thread.core];
    pthread_mutex_unlock(&mutex);
}
void pinParser(bool pinned) {
    pthread_mutex_lock(&mutex);
    if (parser_pinned != pinned) { parser_pinned = pinned; balance(true); }
    pthread_mutex_unlock(&mutex);
}
bool videoActive() {
    pthread_mutex_lock(&mutex);
    bool active = video_active > 0;
    pthread_mutex_unlock(&mutex);
    return active;
}
void writeReport(FILE* file) {
    static const char* role_names[] = {"ui", "audio", "video", "network", "parser", "storage", "prefetch"};
    static const char* fourth_names[] = {"checking", "timeout", "create_failed", "start_failed", "affinity_failed", "wrong_core"};
    const int state = fourth.load();
    const char* cpu3 = state == Confirmed ? "confirmed" : state <= Checking && state >= WrongCore ? fourth_names[Checking - state] : "unknown";
    pthread_mutex_lock(&mutex);
    const auto reported_threads = threads;
    const auto reported_sum = busy_sum;
    const auto reported_max = busy_max;
    const auto samples = report_samples;
    const int min_clock = clock_min, max_clock = clock_max;
    const int audio = audio_active, video = video_active;
    for (auto& thread : threads) { thread.seconds = {}; thread.moves = 0; }
    busy_sum = {}; busy_max = {{-1,-1,-1,-1}}; report_samples = 0; clock_min = clock_max = 0;
    pthread_mutex_unlock(&mutex);
    // Card I/O must not hold the lock used by UI ticks and playback transitions.
    auto mean = [](uint32_t sum, uint32_t n) { return n ? (int)(sum / n) : -1; };
    std::fprintf(file, "cpu_cores samples=%u cpu3=%s arm_mhz_min=%d arm_mhz_max=%d audio=%d video=%d busy_mean=%d,%d,%d,%d busy_max=%d,%d,%d,%d\n",
        samples, cpu3, min_clock, max_clock, audio, video,
        mean(reported_sum[0], samples), mean(reported_sum[1], samples), mean(reported_sum[2], samples), mean(reported_sum[3], samples),
        reported_max[0], reported_max[1], reported_max[2], reported_max[3]);
    for (const auto& thread : reported_threads) {
        if (thread.id < 0) continue;
        std::fprintf(file, "cpu_thread role=%s name=%s core=%d seconds=%u,%u,%u,%u moves=%u\n",
            role_names[static_cast<int>(thread.role)], thread.name[0] ? thread.name : "-", thread.core,
            thread.seconds[0], thread.seconds[1], thread.seconds[2], thread.seconds[3], thread.moves);
    }
}
std::string threadStatus() {
    pthread_mutex_lock(&mutex);
    int ui = 0, audio = 0, video = 0, network = 0, parser = 0, storage = 0, prefetch = 0;
    for (const auto& thread : threads) {
        if (thread.id < 0) continue;
        switch (thread.role) {
            case Role::Ui: ++ui; break;
            case Role::Audio: ++audio; break;
            case Role::Video: ++video; break;
            case Role::Network: ++network; break;
            case Role::Parser: ++parser; break;
            case Role::Storage: ++storage; break;
            case Role::Prefetch: ++prefetch; break;
        }
    }
    pthread_mutex_unlock(&mutex);
    return "Threads UI:" + std::to_string(ui) + " Net:" + std::to_string(network) +
        " Parser:" + std::to_string(parser) + " Audio:" + std::to_string(audio) +
        " Video:" + std::to_string(video) + " Disk:" + std::to_string(storage) +
        " Pre:" + std::to_string(prefetch);
}
}
