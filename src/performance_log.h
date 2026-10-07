#pragma once
#include "performance.h"
#include "td_diagnostics.h"
#include <cstdio>
#include <string>
#include <sys/stat.h>
#ifdef __vita__
#include <psp2/power.h>
#include <psp2/kernel/sysmem.h>
#endif

namespace performance {
struct Display {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    std::array<char, 240> text{};
    int64_t updated = 0;
};
inline Display& display() { static Display value; return value; }
// UI keeps its previous text if the writer is publishing; never waits on it.
inline void readDisplay(std::string& text) {
    auto& value = display();
    if (pthread_mutex_trylock(&value.mutex)) return;
    char copy[240]; std::copy(value.text.begin(), value.text.end(), copy);
    const int64_t updated = value.updated;
    pthread_mutex_unlock(&value.mutex);
    if (!copy[0]) return;
    text = copy;
    char age[40];
    std::snprintf(age, sizeof(age), " | age %.0fs", std::max<int64_t>(0, now()-updated)/1000000.0);
    text += age;
}
inline void writeSnapshot(FILE* file, const Snapshot& snapshot, const char* version) {
    std::fprintf(file, "perf version=%s from_us=%lld until_us=%lld dropped_samples=%llu\n", version,
        (long long)snapshot.from, (long long)snapshot.until, (unsigned long long)snapshot.dropped);
    for (size_t i = 0; i < metric_count; ++i) {
        const auto& h = snapshot.metrics[i];
        if (!h.count) continue;
        std::fprintf(file, "%s n=%llu mean=%llu p50_le=%llu p95_le=%llu p99_le=%llu max=%llu\n", names[i],
            (unsigned long long)h.count, (unsigned long long)(h.sum/h.count),
            (unsigned long long)h.percentile(50), (unsigned long long)h.percentile(95),
            (unsigned long long)h.percentile(99), (unsigned long long)h.maximum);
    }
    for (size_t i = 0; i < gauge_count; ++i)
        std::fprintf(file, "%s current=%llu peak=%llu\n", gauge_names[i],
            (unsigned long long)snapshot.gauges[i].current, (unsigned long long)snapshot.gauges[i].peak);
    if (snapshot.slow_count || snapshot.slow_dropped)
        std::fprintf(file,"slow_events threshold_us=50000 kept=%llu omitted=%llu\n",(unsigned long long)snapshot.slow_count,(unsigned long long)snapshot.slow_dropped);
    for (size_t i=0;i<snapshot.slow_count;++i) {
        const auto& e=snapshot.slow[i];
        std::fprintf(file,"slow_event phase=%s type=%s elapsed_us=%lld at_us=%lld wire_bytes=%llu chats=%llu histories=%llu cpu_start=%d cpu_end=%d\n",
            slow_names[static_cast<size_t>(e.phase)],e.type.data(),(long long)e.elapsed,(long long)e.at,
            (unsigned long long)e.wire_bytes,(unsigned long long)e.chats,(unsigned long long)e.histories,e.cpu_start,e.cpu_end);
    }
}
// Exactly one background cache worker calls this, at most once per 5 seconds.
// The current interval is deliberately not retained on disk errors: RAM stays bounded.
inline bool flush(const char* path, const char* previous, const char* version, int64_t time) {
    if (!enabled) return true;
    auto snapshot = collector().take(time);
    const auto& frame = snapshot.get(Metric::Frame);
    const auto& event = snapshot.get(Metric::EventWait);
    char summary[240];
    char event_text[32];
    if (event.count) std::snprintf(event_text, sizeof(event_text), "<=%.1f", event.percentile(95)/1000.0);
    else std::snprintf(event_text, sizeof(event_text), "n/a");
    std::snprintf(summary, sizeof(summary), "Frame p95<=%.1f max %.1f ms | Events %s ms | Q %llu/%llu/%llu",
        frame.percentile(95)/1000.0, frame.maximum/1000.0, event_text,
        (unsigned long long)snapshot.gauges[0].current,
        (unsigned long long)snapshot.gauges[2].current,
        (unsigned long long)snapshot.gauges[4].current);
    struct stat info{};
    bool ok = true;
    if (!stat(path, &info) && info.st_size >= 1024*1024) ok = std::rename(path, previous) == 0;
    if (ok) {
        FILE* file = std::fopen(path, "ab");
        if (file) {
            writeSnapshot(file, snapshot, version);
            td_diagnostics::write(file);
#ifdef __vita__
            SceKernelFreeMemorySizeInfo memory{}; memory.size = sizeof(memory);
            const int memory_result = sceKernelGetFreeMemorySize(&memory);
            std::fprintf(file, "device cpu_mhz=%d bus_mhz=%d gpu_mhz=%d battery_temp_centi_c=%d memory_result=%d free_user=%d free_cdram=%d free_phycont=%d cdram_images=%lld cdram_glyphs=%lld cdram_video=%lld\n",
                scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(),
                scePowerGetBatteryTemp(), memory_result, memory.size_user, memory.size_cdram, memory.size_phycont,
                (long long)cdram_images.load(), (long long)cdram_glyphs.load(), (long long)cdram_video.load());
#endif
            ok = !std::ferror(file);
            if (std::fclose(file)) ok = false;
        } else ok = false;
    }
    if (!ok) std::snprintf(summary, sizeof(summary), "Performance log: write failed | Frame max %.1f ms", frame.maximum/1000.0);
    auto& value = display();
    pthread_mutex_lock(&value.mutex);
    std::snprintf(value.text.data(), value.text.size(), "%s", summary);
    value.updated = time;
    pthread_mutex_unlock(&value.mutex);
    return ok;
}
}
