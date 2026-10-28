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
// UI publishes TDLib's readable prefix; decoder threads never call TDLib.
class State {
public:
    struct Snapshot { std::string path; int64_t size = 0, prefix = 0; bool complete = false; };
    explicit State(int32_t id) : source("vitagram-stream-" + std::to_string(id)) {}
    void update(std::string path, int64_t size, int64_t offset, int64_t prefix, bool complete) {
        std::lock_guard<std::mutex> lock(mutex);
        value = {std::move(path), std::max<int64_t>(0, size),
                 complete ? std::max<int64_t>(0, size) : offset == 0 ? std::max<int64_t>(0, prefix) : 0, complete};
        changed.notify_all();
    }
    Snapshot snapshot() const { std::lock_guard<std::mutex> lock(mutex); return value; }
    bool ready() const {
        auto s = snapshot();
        return !s.path.empty() && s.size > 0 && s.prefix >= std::min<int64_t>(s.size, 256 * 1024);
    }
    const std::string source; // Stable across TDLib's temporary -> final rename.
    mutable std::mutex mutex;
    std::condition_variable changed;
    Snapshot value;
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
