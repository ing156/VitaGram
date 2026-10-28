#pragma once
#include "performance.h"
#include <array>
#include <mutex>
#include <cstdio>
#include <cstring>

namespace stream_diagnostics {
// Only copied under the lock; neither disk I/O nor waiting holds this mutex.
struct Entry {
    const void* reader = nullptr;
    const char* stage = "idle";
    int64_t at = 0, position = 0, prefix = 0, size = 0;
    int result = 0;
    bool video = false;
    int64_t tail_begin = 0, tail_end = 0;
    bool tail_active = false;
};
inline std::mutex mutex;
inline std::array<Entry, 8> entries{};
inline void update(const void* reader, bool video, const char* stage, int64_t position,
                   int64_t prefix, int64_t size, int result = 0,
                   int64_t tail_begin = 0, int64_t tail_end = 0, bool tail_active = false) {
    if (!performance::enabled) return;
    std::lock_guard<std::mutex> lock(mutex);
    Entry* free = nullptr;
    for (auto& e : entries) {
        if (!e.reader && !free) free = &e;
        if (e.reader != reader) continue;
        if (std::strcmp(e.stage, stage) != 0 || e.position != position) e.at = performance::now();
        e.stage = stage; e.position = position; e.prefix = prefix; e.size = size; e.result = result;
        e.tail_begin = tail_begin; e.tail_end = tail_end; e.tail_active = tail_active;
        return;
    }
    if (free) *free = {reader, stage, performance::now(), position, prefix, size, result, video, tail_begin, tail_end, tail_active};
}
inline void remove(const void* reader) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& e : entries) if (e.reader == reader) e = {};
}
inline void write(FILE* file) {
    std::array<Entry, 8> snapshot;
    { std::lock_guard<std::mutex> lock(mutex); snapshot = entries; }
    for (size_t i = 0; i < snapshot.size(); ++i) {
        const auto& e = snapshot[i];
        if (!e.reader) continue;
        std::fprintf(file, "stream_active slot=%u role=%s stage=%s age_us=%lld position=%lld prefix=%lld size=%lld result=%d tail_begin=%lld tail_end=%lld tail_active=%d\n",
            unsigned(i), e.video ? "video" : "audio_or_media", e.stage,
            (long long)(performance::now() - e.at), (long long)e.position,
            (long long)e.prefix, (long long)e.size, e.result, (long long)e.tail_begin, (long long)e.tail_end, e.tail_active ? 1 : 0);
    }
}
}
