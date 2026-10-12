#pragma once
#include <array>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <sys/stat.h>
#include "diag_log.h"

// UI/Network/Parser only append to this small RAM ring. The cache worker writes
// it to disk. No query text, result titles, credentials or account IDs are logged.
namespace gif_diagnostics {
struct Event {
    char stage[24]{}, kind[24]{}, reason[24]{};
    int64_t at = 0, generation = 0, value = 0, bytes = 0;
    int item = -1, file = 0, width = 0, height = 0;
};
struct Ring { std::mutex mutex; std::array<Event,128> events{}; size_t count = 0; unsigned dropped = 0; };
inline Ring& ring() { static Ring value; return value; }
inline void record(const char* stage, const char* kind, int64_t generation, int64_t at, int64_t value = 0) {
    if (!diag_log::enabled(diag_log::Kind::Gif)) return;
    auto& r = ring(); std::lock_guard<std::mutex> lock(r.mutex);
    if (r.count == r.events.size()) { ++r.dropped; return; }
    auto& event = r.events[r.count++];
    event = Event{};
    std::snprintf(event.stage, sizeof(event.stage), "%s", stage);
    std::snprintf(event.kind, sizeof(event.kind), "%s", kind);
    event.at = at; event.generation = generation; event.value = value;
}
inline void raw(const char* stage, const char* text, int64_t at) {
    if (!text) return;
    const size_t length = std::strlen(text);
    const char* tail = text + (length > 400 ? length - 400 : 0);
    const char* extra = std::strstr(tail, "\"@extra\":{");
    const char* kind = extra ? std::strstr(extra, "\"kind\":\"gif_") : nullptr;
    if (!kind) return;
    kind += 8;
    const char* end = std::strchr(kind, '"');
    if (!end || end - kind >= 24) return;
    char name[24]{}; std::memcpy(name, kind, end - kind);
    const char* generation = std::strstr(extra, "\"generation\":");
    record(stage, name, generation ? std::strtoll(generation + 13, nullptr, 10) : 0, at);
}
inline void preview(const char* stage, const char* format, int64_t generation, int item,
                    int file, int width, int height, int64_t bytes, const char* reason,
                    int64_t at, int64_t value = 0) {
    if (!diag_log::enabled(diag_log::Kind::Gif)) return;
    auto& r = ring(); std::lock_guard<std::mutex> lock(r.mutex);
    if (r.count == r.events.size()) { ++r.dropped; return; }
    auto& e = r.events[r.count++]; e = Event{};
    std::snprintf(e.stage, sizeof(e.stage), "%s", stage);
    std::snprintf(e.kind, sizeof(e.kind), "%s", format);
    std::snprintf(e.reason, sizeof(e.reason), "%s", reason);
    e.generation = generation; e.item = item; e.file = file;
    e.width = width; e.height = height; e.bytes = bytes; e.at = at; e.value = value;
}
inline void flush() {
    std::array<Event,128> events; size_t count; unsigned dropped;
    auto& r = ring();
    const bool write = diag_log::enabled(diag_log::Kind::Gif);
    { std::lock_guard<std::mutex> lock(r.mutex);
      count = r.count; dropped = r.dropped; events = r.events; r.count = 0; r.dropped = 0; }
    if (!write || (!count && !dropped)) return;
    const char* path = "ux0:data/vitagram/gif.log";
    struct stat info{};
    if (!stat(path, &info) && info.st_size >= 256 * 1024)
        if (std::rename(path, "ux0:data/vitagram/gif.previous.log")) return;
    if (FILE* file = std::fopen(path, "ab")) {
        for (size_t i = 0; i < count; ++i) {
            const auto& e = events[i];
            if (e.item < 0) std::fprintf(file, "gif at_us=%lld stage=%s kind=%s generation=%lld value=%lld\n",
                (long long)e.at, e.stage, e.kind, (long long)e.generation, (long long)e.value);
            if (e.item >= 0) std::fprintf(file, "preview at_us=%lld stage=%s generation=%lld item=%d file=%d format=%s width=%d height=%d bytes=%lld reason=%s value=%lld\n",
                (long long)e.at, e.stage, (long long)e.generation, e.item, e.file, e.kind,
                e.width, e.height, (long long)e.bytes, e.reason, (long long)e.value);
        }
        if (dropped) std::fprintf(file, "gif omitted=%u\n", dropped);
        std::fclose(file);
    }
}
}
