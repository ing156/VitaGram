#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace download_stream {
constexpr int64_t video_start_bytes = 8 * 1024 * 1024;
constexpr int64_t video_resume_bytes = 4 * 1024 * 1024;
constexpr int64_t mp4_tail_bytes = 8 * 1024 * 1024;
// Vita can spend nearly a minute extending a download to its trailing metadata.
constexpr int64_t tail_timeout_us = 120000000;
inline std::atomic<unsigned> next_generation{0};
// UI publishes TDLib's readable prefix; decoder threads never call TDLib.
class State {
public:
    struct Snapshot {
        std::string path;
        int64_t size = 0, prefix = 0;
        bool complete = false;
        int64_t tail_begin = 0, tail_end = 0;
        int64_t available(int64_t position) const {
            if (position < 0 || position >= size) return 0;
            const int64_t end = complete ? size : position < prefix ? prefix :
                position >= tail_begin && position < tail_end ? tail_end : position;
            return std::max<int64_t>(0, std::min(size, end) - position);
        }
    };
    explicit State(int32_t id) : source("vitagram-stream-" + std::to_string(id)) {}
    void update(std::string path, int64_t size, int64_t offset, int64_t prefix, bool complete) {
        std::lock_guard<std::mutex> lock(mutex);
        size = std::max<int64_t>(0, size);
        prefix = std::clamp<int64_t>(prefix, 0, size - std::clamp<int64_t>(offset, 0, size));
        const bool replaced = value.size != size || (!complete && value.path != path);
        if (replaced) {
            value.tail_begin = value.tail_end = 0;
            if (attempted_) failed_ = true;
            ranged_ = false;
        }
        const int64_t head = complete ? size : offset == 0 ? prefix :
            ranged_ && !replaced ? value.prefix : 0;
        if (ranged_ && !failed_ && offset == requested_ && !complete && prefix > 0) {
            const int64_t end = offset + prefix;
            if (value.tail_end >= offset && end >= value.tail_begin) {
                value.tail_begin = std::min(value.tail_begin, offset);
                value.tail_end = std::max(value.tail_end, end);
            } else { value.tail_begin = offset; value.tail_end = end; }
        }
        value.path = std::move(path); value.size = size; value.prefix = head; value.complete = complete;
        changed.notify_all();
    }
    Snapshot snapshot() const { std::lock_guard<std::mutex> lock(mutex); return value; }
    bool ready() const {
        auto s = snapshot();
        return !s.path.empty() && s.size > 0 && s.prefix >= std::min<int64_t>(s.size, 256 * 1024);
    }
    void beginOpen() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!opening_ && !active_) { attempted_ = failed_ = false; requested_ = -1; }
        ++opening_;
    }
    void endOpen() { std::lock_guard<std::mutex> lock(mutex); if (opening_) --opening_; }
    void requestTail(int64_t position) {
        std::lock_guard<std::mutex> lock(mutex);
        // shortcut: one bounded MP4 tail per opening; other gaps keep sequential download until general range seeking is needed.
        if (!opening_ || attempted_ || value.complete || position <= value.prefix + 1024 * 1024 ||
            position < std::max<int64_t>(0, value.size - mp4_tail_bytes) || position >= value.size) return;
        const int64_t aligned = position / (64 * 1024) * (64 * 1024);
        if (value.size - aligned > mp4_tail_bytes) return;
        if (requested_ < 0) requested_ = aligned;
    }
    // UI thread only: -2 drops an unsent tail, -1 means no command, 0 restores full download.
    int64_t pollTail(int64_t now, bool permitted) {
        std::lock_guard<std::mutex> lock(mutex);
        if (active_) {
            if (!permitted || value.complete) { active_ = false; return permitted ? -1 : -2; }
            if (!opening_ || failed_ || (value.tail_begin <= requested_ && value.tail_end == value.size) ||
                now - started_ >= tail_timeout_us) {
                active_ = false;
                return 0;
            }
        } else if (!attempted_ && requested_ >= 0) {
            if (!permitted || !opening_) { requested_ = -1; return -1; }
            attempted_ = active_ = ranged_ = true; started_ = now;
            generation = ++next_generation;
            return requested_;
        }
        return -1;
    }
    bool tailActive() const { std::lock_guard<std::mutex> lock(mutex); return active_; }
    void tailError(int64_t offset) {
        std::lock_guard<std::mutex> lock(mutex);
        if (active_ && offset == requested_) failed_ = true;
    }
    unsigned generation = ++next_generation; // UI command token: reject replies from earlier attempts or retired streams.
    const std::string source; // Stable across TDLib's temporary -> final rename.
    mutable std::mutex mutex;
    std::condition_variable changed;
    Snapshot value;
private:
    int64_t requested_ = -1, started_ = 0;
    unsigned opening_ = 0;
    bool attempted_ = false, active_ = false, failed_ = false, ranged_ = false;
};
using Shared = std::shared_ptr<State>;
// Prevent sealing a download while a progressive reader still has it open,
// including audio-only playback and waiting for more network data.
inline std::atomic<unsigned> readers{0};
inline bool reading() { return readers.load() != 0; }
class Pin {
public:
    explicit Pin(bool active) : active_(active) { if (active_) ++readers; }
    ~Pin() { if (active_) --readers; }
    Pin(const Pin&) = delete;
    Pin& operator=(const Pin&) = delete;
private:
    bool active_;
};
}
