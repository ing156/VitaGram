#pragma once
#include "performance.h"
#include <cstdio>
#include <cstring>
#include <array>
#include <algorithm>

// All TD calls belong to the network worker. Only this worker publishes the
// nested phase; the log worker reads a single coherent, allocation-free value.
namespace td_diagnostics {
constexpr uint64_t time_mask = (uint64_t{1} << 56) - 1;
inline std::atomic<uint64_t>& active() { static std::atomic<uint64_t> value{0}; return value; }
inline const char* name(unsigned phase) {
    constexpr const char* names[] = {"idle", "actor", "poll", "socket_read", "socket_write", "factorize", "dh_check", "dns",
                                    "file_io", "log_write", "socket_connect", "file_sync", "file_meta", "file_read", "file_write", "file_pread", "file_pwrite"};
    return phase < sizeof(names)/sizeof(*names) ? names[phase] : "unknown";
}
// Directory label of the current TD file operation. Network worker only.
inline std::array<char,64>& path_label() { static thread_local std::array<char,64> label{}; return label; }
// Keeps the last two directory components; never the file name.
inline void path(const char* full) {
    if (!performance::enabled) return;
    const char* end=std::strrchr(full,'/');
    auto& label=path_label();
    if (!end) { std::snprintf(label.data(),label.size(),"no_dir"); return; }
    const char* begin=end; int slashes=0;
    while (begin>full && slashes<2) { --begin; if (*begin=='/' || *begin==':') ++slashes; }
    if (*begin=='/' || *begin==':') ++begin;
    size_t length=std::min<size_t>(end-begin,label.size()-1);
    for (size_t i=0;i<length;++i) {
        unsigned char c=begin[i];
        label[i]=((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_') ? char(c) : '_';
    }
    label[length]=0;
}
struct FileContext {
    std::array<char,8> mount{};
    int fd = -1;
    int64_t offset = -1;
    uint64_t bytes = 0;
};
inline thread_local FileContext file_context;
inline void file(const char* full, int fd, int64_t offset, uint64_t bytes) {
    if (!performance::enabled) return;
    path(full);
    file_context = {};
    file_context.fd = fd; file_context.offset = offset; file_context.bytes = bytes;
    const char* colon = std::strchr(full, ':');
    if (colon && colon - full < 7) {
        for (const char* c = full; c < colon; ++c) {
            if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'))) { file_context.mount = {}; break; }
            file_context.mount[c - full] = *c;
        }
    }
}
inline void describe_file(performance::SlowEvent& event) {
    if (file_context.fd < 0) return;
    std::snprintf(event.file_dir.data(), event.file_dir.size(), "%s%s%s",
        file_context.mount.data(), file_context.mount[0] ? ":" : "", path_label()[0] ? path_label().data() : "unknown");
    event.file_fd = file_context.fd; event.file_offset = file_context.offset; event.file_bytes = file_context.bytes;
}
inline uint64_t enter(unsigned phase) {
    if (!performance::enabled) return 0;
    return active().exchange((uint64_t{phase} << 56) | (performance::now() & time_mask), std::memory_order_relaxed);
}
inline void leave(uint64_t previous) {
    if (!performance::enabled) return;
    auto finished = active().exchange(previous, std::memory_order_relaxed);
    const unsigned phase = finished >> 56;
    if (!phase || phase > 16) return;
    // Phases 1..10 map contiguously from TdActor; later phases were appended.
    const auto metric = phase >= 13 ? performance::Metric::TdFileIo : phase == 11 ? performance::Metric::TdFileSync : phase == 12 ? performance::Metric::TdFileMeta :
        static_cast<performance::Metric>(static_cast<unsigned>(performance::Metric::TdActor) + phase - 1);
    const auto elapsed=(performance::now() - (finished & time_mask)) & time_mask;
    performance::observe(metric, elapsed);
    if (phase >= 13 && elapsed >= 50000) {
        performance::SlowEvent event;
        event.phase = performance::SlowPhase::Storage;
        std::snprintf(event.type.data(), event.type.size(), "td_%s", name(phase));
        event.elapsed = static_cast<int64_t>(elapsed); event.at = performance::now();
        describe_file(event);
        performance::collector().slow(event);
    }
    if (phase == 12) {
        // Split the lookup cost: sticker paths (bucketed since layout 3) vs the rest.
        const char* l=path_label().data();
        using M=performance::Metric;
        M group = std::strstr(l, "stickers") ? M::TdFileMetaSticker : std::strstr(l, "temp") ? M::TdFileMetaTemp :
            std::strstr(l, "photos") ? M::TdFileMetaPhotos : std::strstr(l, "thumbnails") ? M::TdFileMetaThumbs :
            !std::strcmp(l, "vitagram_db") ? M::TdFileMetaDb : M::TdFileMetaOther;
        performance::observe(group, elapsed);
    }
    if (phase == 11 || phase == 12) {
        auto& label=path_label();
        if (elapsed >= 50000) {
            performance::SlowEvent event;
            event.phase = phase == 11 ? performance::SlowPhase::TdFileSync : performance::SlowPhase::TdFileMeta;
            std::snprintf(event.type.data(), event.type.size(), "%s", label[0] ? label.data() : "unknown");
            event.elapsed = static_cast<int64_t>(elapsed); event.at = performance::now();
            describe_file(event);
            performance::collector().slow(event);
        }
        label[0]=0;
    }
    if (phase == 9 || phase >= 11) file_context = {};
}
inline void write(FILE* file) {
    const auto state = active().load(std::memory_order_relaxed);
    const unsigned phase = state >> 56;
    const auto age = phase ? (performance::now() - (state & time_mask)) & time_mask : 0;
    std::fprintf(file, "td_active phase=%s age_us=%llu\n", name(phase), (unsigned long long)age);
}
}
