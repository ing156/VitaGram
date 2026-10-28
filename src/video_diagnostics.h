#pragma once
#include "performance.h"
#include <array>
#include <cstdio>
#include <mutex>
#include <cstring>

namespace video_diagnostics {
struct Playback {
    int64_t packet_dts = -1, packet_pts = -1, frame_pts = -1, due = -1;
    bool paused = false, audio_playing = false, clock_ready = false;
    bool buffering = false, stalled = false, lagging = false;
};
struct Entry {
    const void* job = nullptr;
    const char* stage = "idle";
    int64_t at = 0, clock = 0, position = 0;
    unsigned packets = 0, frames = 0;
    Playback playback;
    int64_t updated_at = 0;
};
inline std::mutex mutex;
inline std::array<Entry, 4> entries{};
inline void update(const void* job, const char* stage, unsigned packets, unsigned frames, int64_t clock, int64_t position, Playback playback = {}) {
    if (!performance::enabled) return;
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const int64_t now = performance::now();
    Entry* free = nullptr;
    for (auto& entry : entries) {
        if (!entry.job && !free) free = &entry;
        if (entry.job != job) continue;
        if (std::strcmp(entry.stage, stage)) entry.at = now;
        entry.stage = stage; entry.packets = packets; entry.frames = frames; entry.clock = clock; entry.position = position;
        entry.playback = playback; entry.updated_at = now;
        return;
    }
    if (free) *free = {job, stage, now, clock, position, packets, frames, playback, now};
}
inline void remove(const void* job) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& entry : entries) if (entry.job == job) entry = {};
}
inline void write(FILE* file) {
    std::array<Entry, 4> snapshot;
    { std::lock_guard<std::mutex> lock(mutex); snapshot = entries; }
    for (const auto& entry : snapshot) if (entry.job)
        std::fprintf(file, "video_decoder stage=%s age_us=%lld packets=%u frames=%u audio_clock_at_stage_us=%lld video_position_at_stage_us=%lld snapshot_age_us=%lld packet_dts_us=%lld packet_pts_us=%lld frame_pts_us=%lld due_us=%lld paused=%d audio_playing=%d clock_ready=%d audio_buffering=%d frame_stalled=%d frame_lagging=%d\n",
            entry.stage, (long long)(performance::now() - entry.at), entry.packets, entry.frames, (long long)entry.clock, (long long)entry.position,
            (long long)(performance::now() - entry.updated_at), (long long)entry.playback.packet_dts,
            (long long)entry.playback.packet_pts, (long long)entry.playback.frame_pts, (long long)entry.playback.due,
            entry.playback.paused, entry.playback.audio_playing, entry.playback.clock_ready,
            entry.playback.buffering, entry.playback.stalled, entry.playback.lagging);
}
}
