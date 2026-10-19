#pragma once
#include "diag_log.h"
#include "download_stream.h"
#include "performance.h"
#include <algorithm>
#include <array>
#include <cstdio>

namespace audio_startup_trace {
struct Event {
    const void* player = nullptr;
    const char* stage = nullptr;
    unsigned generation = 0;
    int result = 0;
    int64_t at = 0, total = 0, size = -1, prefix = -1;
    int complete = -1;
    int32_t file = 0;
};
inline constexpr size_t capacity = 128;
inline pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
inline std::array<Event, capacity> events{};
inline size_t count = 0;
inline std::atomic<unsigned> dropped{0};

// Stage names are string literals. Producers never write to disk or wait for the writer.
inline void record(const void* player, unsigned generation, const char* stage, int result = 0,
                   int64_t origin = 0, const download_stream::Shared& stream = nullptr, int32_t file = 0) {
    if (!performance::enabled || !diag_log::enabled(diag_log::Kind::Performance)) return;
    Event event;
    event.player = player; event.generation = generation; event.stage = stage; event.result = result;
    event.at = performance::now(); event.total = origin ? event.at - origin : 0; event.file = file;
    if (stream) {
        const auto state = stream->snapshot();
        event.size = state.size; event.prefix = state.prefix; event.complete = state.complete;
    }
    if (pthread_mutex_trylock(&mutex)) { ++dropped; return; }
    if (count < capacity) events[count++] = event;
    else ++dropped;
    pthread_mutex_unlock(&mutex);
}

// Called by the existing performance-log worker, including while a player is stuck in a stage.
inline void write(FILE* log) {
    std::array<Event, capacity> snapshot;
    pthread_mutex_lock(&mutex);
    const size_t kept = count;
    std::copy_n(events.begin(), kept, snapshot.begin());
    count = 0;
    const unsigned omitted = dropped.exchange(0);
    pthread_mutex_unlock(&mutex);
    if (kept || omitted) std::fprintf(log, "audio_startup_events kept=%u dropped=%u\n", static_cast<unsigned>(kept), omitted);
    for (size_t i = 0; i < kept; ++i) {
        const auto& e = snapshot[i];
        std::fprintf(log, "audio_startup player=%p generation=%u stage=%s at_us=%lld total_us=%lld result=%d file_id=%d size=%lld prefix=%lld complete=%d\n",
            e.player, e.generation, e.stage, (long long)e.at, (long long)e.total, e.result, e.file,
            (long long)e.size, (long long)e.prefix, e.complete);
    }
}
}
