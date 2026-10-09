#include "vault_readahead.h"
#include "vault_crypto.h"
#include <unistd.h>
#include <algorithm>

#include <cstring>

namespace vault {
namespace {
void* (*g_enter)() = nullptr;
void (*g_leave)(void*) = nullptr;
}

void ReadAhead::set_thread_hooks(void* (*enter)(), void (*leave)(void*)) { g_enter = enter; g_leave = leave; }

bool ReadAhead::open(std::unique_ptr<Reader> reader) {
    close();
    if (!reader || !reader->is_open()) return false;
    reader_ = std::move(reader);
    size_ = reader_->size();
    pos_ = fill_pos_ = 0; seek_target_ = -1;
    stop_ = eof_ = failed_ = false;
    ring_.clear();
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 128 * 1024);
    started_ = pthread_create(&thread_, &attributes, entry, this) == 0;
    pthread_attr_destroy(&attributes);
    if (!started_) { reader_.reset(); return false; }
    return true;
}

void ReadAhead::close() {
    if (started_) {
        pthread_mutex_lock(&mutex_);
        stop_ = true;
        pthread_mutex_unlock(&mutex_);
        pthread_join(thread_, nullptr);
        started_ = false;
    }
    for (auto& block : ring_) if (!block.data.empty()) vault_crypto::wipe(block.data.data(), block.data.size());
    ring_.clear();
    if (reader_) final_stats_ = {reader_->stat_bytes(), reader_->stat_micros(), reader_->stat_io_micros()};
    reader_.reset();
}

ReadAhead::Stats ReadAhead::stats() const {
    if (reader_) return {reader_->stat_bytes(), reader_->stat_micros(), reader_->stat_io_micros()};
    return final_stats_;
}

void* ReadAhead::entry(void* self) {
    void* token = g_enter ? g_enter() : nullptr;
    static_cast<ReadAhead*>(self)->run();
    if (g_leave) g_leave(token);
    return nullptr;
}

void ReadAhead::run() {
    for (;;) {
        pthread_mutex_lock(&mutex_);
        if (stop_) { pthread_mutex_unlock(&mutex_); return; }
        if (seek_target_ >= 0) {
            const int64_t target = seek_target_;
            seek_target_ = -1;
            ring_.clear(); eof_ = false; failed_ = false; fill_pos_ = target;
            pthread_mutex_unlock(&mutex_);
            reader_->seek(target, SEEK_SET);
            continue;
        }
        if (ring_.size() >= kBlocks || eof_ || failed_ || fill_pos_ >= size_) {
            if (fill_pos_ >= size_) eof_ = true;
            pthread_mutex_unlock(&mutex_);
            usleep(2000);
            continue;
        }
        pthread_mutex_unlock(&mutex_);
        Block block;
        block.data.resize(kBlock);
        const int64_t n = reader_->read(block.data.data(), kBlock); // card + decryption, outside the lock
        pthread_mutex_lock(&mutex_);
        if (seek_target_ >= 0 || stop_) { pthread_mutex_unlock(&mutex_); continue; } // stale
        if (n < 0) failed_ = true;
        else if (n == 0) eof_ = true;
        else { block.data.resize((size_t)n); fill_pos_ += n; ring_.push_back(std::move(block)); }
        pthread_mutex_unlock(&mutex_);
    }
}

int64_t ReadAhead::read(void* buffer, size_t length) {
    if (!started_) return -1;
    if (!length) return 0;
    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    for (;;) {
        pthread_mutex_lock(&mutex_);
        while (done < length && !ring_.empty()) {
            Block& block = ring_.front();
            size_t n = std::min(length - done, block.data.size() - block.used);
            std::memcpy(out + done, block.data.data() + block.used, n);
            block.used += n; done += n; pos_ += (int64_t)n;
            if (block.used == block.data.size()) ring_.pop_front();
        }
        const bool at_end = pos_ >= size_;
        const bool failed = failed_ && ring_.empty() && seek_target_ < 0;
        pthread_mutex_unlock(&mutex_);
        if (done > 0) break; // enough: partial reads are fine for the callers
        if (at_end) return 0;
        if (failed) return -1;
        usleep(1000); // helper has not produced the next block yet
    }
    return (int64_t)done;
}

int64_t ReadAhead::seek(int64_t offset, int whence) {
    if (!started_) return -1;
    pthread_mutex_lock(&mutex_);
    const int64_t target = whence == SEEK_SET ? offset : whence == SEEK_CUR ? pos_ + offset : size_ + offset;
    if (target < 0) { pthread_mutex_unlock(&mutex_); return -1; }
    if (seek_target_ < 0 && target >= pos_ && target <= fill_pos_) {
        // Inside what is already decrypted: drop the blocks in front.
        while (pos_ < target) {
            Block& block = ring_.front();
            size_t n = (size_t)std::min<int64_t>(target - pos_, (int64_t)(block.data.size() - block.used));
            block.used += n; pos_ += (int64_t)n;
            if (block.used == block.data.size()) ring_.pop_front();
        }
    } else {
        ring_.clear();
        seek_target_ = target; pos_ = target;
    }
    pthread_mutex_unlock(&mutex_);
    return target;
}
}
