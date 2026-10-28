#pragma once
#include "download_stream.h"
#include "performance.h"
#include <pthread.h>
#include <deque>
#include <vector>
#include <cstring>
#include <chrono>
extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}
#ifdef __vita__
#include "cpu_scheduler.h"
#endif

// Bounded RAM queue: disk stalls must not stop decoding bytes already read ahead.
class DownloadReadAhead {
public:
    using Read = int (*)(void*, uint8_t*, int, int64_t);
    ~DownloadReadAhead() { close(); }
    bool open(download_stream::Shared state, Read read, void* opaque) {
        state_ = std::move(state); read_ = read; opaque_ = opaque;
        const auto snapshot = state_->snapshot();
        complete_size_ = snapshot.complete ? snapshot.size : 0;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 128 * 1024);
        started_ = pthread_create(&thread_, &attr, entry, this) == 0;
        pthread_attr_destroy(&attr);
        return started_;
    }
    void close() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; changed_.notify_all(); }
        if (started_) pthread_join(thread_, nullptr);
        started_ = false;
        blocks_.clear(); state_.reset();
    }
    int read(uint8_t* out, int count, int64_t position, AVIOInterruptCB interrupt) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!active_ || position < position_ || position > filled_) {
            blocks_.clear(); position_ = filled_ = position; error_ = 0; active_ = true; ++generation_;
        }
        // Forward seeks can reuse data that is already in RAM.
        while (position_ < position && !blocks_.empty()) {
            auto& block = blocks_.front();
            const auto skip = std::min<int64_t>(position - position_, block.bytes.size() - block.used);
            position_ += skip; block.used += skip;
            if (block.used == block.bytes.size()) blocks_.pop_front();
        }
        changed_.notify_all();
        for (;;) {
            if (interrupt.callback && interrupt.callback(interrupt.opaque)) return AVERROR_EXIT;
            if (!blocks_.empty()) {
                auto& block = blocks_.front();
                const int size = std::min<size_t>(count, block.bytes.size() - block.used);
                std::memcpy(out, block.bytes.data() + block.used, size);
                block.used += size; position_ += size;
                if (block.used == block.bytes.size()) blocks_.pop_front();
                changed_.notify_all();
                return size;
            }
            if (error_) return error_;
            changed_.wait_for(lock, std::chrono::milliseconds(20));
        }
    }
private:
    struct Block { std::vector<uint8_t> bytes; size_t used = 0; };
    static void* entry(void* self) { static_cast<DownloadReadAhead*>(self)->run(); return nullptr; }
    void run() {
#ifdef __vita__
        cpu_scheduler::Thread scheduled(cpu_scheduler::Role::Prefetch);
#endif
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            const auto snapshot = state_->snapshot();
            const auto available = (complete_size_ ? complete_size_ : std::min(snapshot.size, snapshot.prefix)) - filled_;
            if (!active_ || error_ || blocks_.size() >= 16 || available <= 0) {
                changed_.wait_for(lock, std::chrono::milliseconds(20));
                continue;
            }
            const auto generation = generation_;
            const auto offset = filled_;
            Block block; block.bytes.resize(std::min<int64_t>(256 * 1024, available));
            lock.unlock();
            int got;
            {
                performance::Scope reading(performance::Metric::StreamRead);
                got = read_(opaque_, block.bytes.data(), block.bytes.size(), offset);
            }
            lock.lock();
            if (generation != generation_ || stop_) continue;
            if (got <= 0) error_ = got < 0 ? got : AVERROR(EIO);
            else { block.bytes.resize(got); filled_ += got; blocks_.push_back(std::move(block)); }
            changed_.notify_all();
        }
    }
    download_stream::Shared state_;
    Read read_ = nullptr;
    void* opaque_ = nullptr;
    pthread_t thread_{};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Block> blocks_;
    int64_t position_ = 0, filled_ = 0, complete_size_ = 0;
    unsigned generation_ = 0;
    int error_ = 0;
    bool active_ = false, stop_ = false, started_ = false;
};
