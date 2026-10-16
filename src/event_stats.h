#pragma once
#include "performance.h"
#include <pthread.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Startup diagnostics: parse (Parser thread) and process (UI thread) time per TDLib event type,
// collected until the first live chat list and written once to the performance log.
namespace event_stats {
// Parse/Process are whole events; the rest are parts of them: Raw (raw compaction) and Json (Jansson)
// inside Parse, Remember (UiCache::remember), Lock (its mutex wait) and Media (media cache) inside Process.
enum Phase { Parse = 0, Process, Raw, Json, Remember, Lock, Media, PhaseCount };
struct Entry { char type[40] = {}; uint32_t count[PhaseCount] = {}; uint64_t us[PhaseCount] = {}; };
constexpr size_t kTypes = 96; // The last slot collects the overflow as "other".
struct State {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    std::array<Entry, kTypes> entries{};
    size_t used = 0;
    std::atomic<bool> active{performance::enabled}, pending{false};
};
inline State& state() { static State s; return s; }
inline bool active() { return performance::enabled && state().active.load(std::memory_order_relaxed); }

inline void add(const char* type, Phase phase, int64_t us) {
    if (!active() || us < 0) return;
    if (!type || !*type) type = "unknown";
    auto& s = state();
    pthread_mutex_lock(&s.mutex);
    Entry* entry = nullptr;
    for (size_t i = 0; i < s.used; ++i)
        if (!std::strncmp(s.entries[i].type, type, sizeof(entry->type) - 1)) { entry = &s.entries[i]; break; }
    if (!entry) {
        if (s.used < kTypes - 1) entry = &s.entries[s.used++];
        else { entry = &s.entries[kTypes - 1]; type = "other"; s.used = kTypes; }
        if (!entry->type[0]) std::strncpy(entry->type, type, sizeof(entry->type) - 1);
    }
    ++entry->count[phase]; entry->us[phase] += static_cast<uint64_t>(us);
    pthread_mutex_unlock(&s.mutex);
}

// Times one event over all return paths; the type is copied because the event may be freed first.
class Scope {
public:
    Scope(const char* type, Phase phase) : phase_(phase), start_(active() ? performance::now() : 0) {
        if (start_ && type) std::strncpy(type_, type, sizeof(type_) - 1);
    }
    ~Scope() { if (start_) add(type_, phase_, performance::now() - start_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Phase phase_; int64_t start_; char type_[40] = {};
};

// The first live chat list arrived: collection ends and the next log write reports it.
inline void stop() {
    if (!performance::enabled) return;
    auto& s = state();
    if (s.active.exchange(false)) s.pending = true;
}

// Performance log writer: the top types by total time, once.
inline void write(FILE* file, size_t top = 12) {
    auto& s = state();
    if (!s.pending.exchange(false)) return;
    pthread_mutex_lock(&s.mutex);
    std::array<Entry, kTypes> sorted = s.entries;
    const size_t used = std::min(s.used, kTypes);
    pthread_mutex_unlock(&s.mutex);
    auto total = [](const Entry& e) { return e.us[Parse] + e.us[Process]; };
    std::sort(sorted.begin(), sorted.begin() + used, [&](const Entry& a, const Entry& b) { return total(a) > total(b); });
    uint64_t events = 0, parse = 0, process = 0;
    for (size_t i = 0; i < used; ++i) { events += sorted[i].count[Parse]; parse += sorted[i].us[Parse]; process += sorted[i].us[Process]; }
    std::fprintf(file, "event_types window=startup types=%u events=%llu parse_us=%llu process_us=%llu\n", (unsigned)used,
        (unsigned long long)events, (unsigned long long)parse, (unsigned long long)process);
    for (size_t i = 0; i < used && i < top; ++i) {
        const auto& e = sorted[i];
        std::fprintf(file, "event_type type=%s parsed=%u parse_us=%llu raw_us=%llu json_us=%llu processed=%u process_us=%llu"
            " remember_us=%llu lock_us=%llu media_us=%llu\n", e.type, (unsigned)e.count[Parse], (unsigned long long)e.us[Parse],
            (unsigned long long)e.us[Raw], (unsigned long long)e.us[Json], (unsigned)e.count[Process], (unsigned long long)e.us[Process],
            (unsigned long long)e.us[Remember], (unsigned long long)e.us[Lock], (unsigned long long)e.us[Media]);
    }
}
}
