#pragma once
#include "download_stream.h"
#include "performance.h"
#include "stream_diagnostics.h"
#include <chrono>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#if defined(__vita__) || defined(VITAGRAM_TEST_SCEIO)
#include <psp2/io/fcntl.h>
#endif
extern "C" {
#include <libavformat/avformat.h>
}

#include "download_readahead.h"

// Bounded disk reads of a growing file. An unavailable byte is a wait, never EOF.
class DownloadAvio {
public:
    DownloadAvio() = default;
    DownloadAvio(const DownloadAvio&) = delete;
    DownloadAvio& operator=(const DownloadAvio&) = delete;
    ~DownloadAvio() { release(); }
    int attach(AVFormatContext* format, download_stream::Shared state, std::atomic<bool>* video_waiting = nullptr,
               int64_t resume_bytes = 0, int64_t startup_bytes = 0, bool readahead = false) {
        release();
        state_ = std::move(state);
        video_waiting_ = video_waiting;
        resume_bytes_ = std::max<int64_t>(0, resume_bytes);
        interrupt_ = format->interrupt_callback;
        if (startup_bytes > 0) {
            auto progress_at = std::chrono::steady_clock::now();
            int64_t previous_prefix = -1;
            bool waiting = false;
            for (;;) {
                if (interrupt_.callback && interrupt_.callback(interrupt_.opaque)) {
                    if (waiting && video_waiting_) *video_waiting_ = false;
                    release(); return AVERROR_EXIT;
                }
                const auto s = state_->snapshot();
                if (s.size <= 0) {
                    if (waiting && video_waiting_) *video_waiting_ = false;
                    release(); return AVERROR(EIO);
                }
                if (s.complete || s.prefix >= std::min(s.size, startup_bytes)) break;
                if (!waiting && video_waiting_) *video_waiting_ = true;
                waiting = true;
                if (s.prefix != previous_prefix) {
                    previous_prefix = s.prefix; progress_at = std::chrono::steady_clock::now();
                    stream_diagnostics::update(this, video_waiting_ != nullptr, "startup_buffer", 0, s.prefix, s.size);
                }
                if (std::chrono::steady_clock::now() - progress_at > std::chrono::seconds(60)) {
                    if (video_waiting_) *video_waiting_ = false;
                    release(); return AVERROR(ETIMEDOUT);
                }
                std::unique_lock<std::mutex> lock(state_->mutex);
                state_->changed.wait_for(lock, std::chrono::milliseconds(20));
            }
            if (waiting && video_waiting_) *video_waiting_ = false;
            if (waiting) stream_diagnostics::remove(this);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        for (;;) {
            if (interrupt_.callback && interrupt_.callback(interrupt_.opaque)) { release(); return AVERROR_EXIT; }
            auto s = state_->snapshot();
            if (s.size <= 0) { release(); return AVERROR(EIO); }
#if defined(__vita__) || defined(VITAGRAM_TEST_SCEIO)
            fd_ = sceIoOpen(s.path.c_str(), SCE_O_RDONLY, 0);
#else
            fd_ = ::open(s.path.c_str(), O_RDONLY);
#endif
            if (fd_ >= 0) { if (s.complete) complete_size_ = s.size; break; }
            // TDLib can rename the temporary path just before its completion update arrives.
            if (s.complete || std::chrono::steady_clock::now() >= deadline) { release(); return AVERROR(EIO); }
            std::unique_lock<std::mutex> lock(state_->mutex);
            state_->changed.wait_for(lock, std::chrono::milliseconds(20));
        }
        const int buffer_size = resume_bytes_ ? 256 * 1024 : 64 * 1024;
        auto* buffer = static_cast<uint8_t*>(av_malloc(buffer_size));
        if (!buffer) { release(); return AVERROR(ENOMEM); }
        io_ = avio_alloc_context(buffer, buffer_size, 0, this, read, nullptr, seek);
        if (!io_) { av_free(buffer); release(); return AVERROR(ENOMEM); }
        ++download_stream::readers; pinned_ = true;
        format->pb = io_;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        const auto opened = state_->snapshot();
        if (!opened.complete && opened.prefix >= 8 && !(format->flags & AVFMT_FLAG_IGNIDX)) {
            uint8_t header[8]{};
#if defined(__vita__) || defined(VITAGRAM_TEST_SCEIO)
            int got;
            do { got = sceIoPread(fd_, header, sizeof(header), 0); }
            while (static_cast<uint32_t>(got) == 0x80010004u);
#else
            ssize_t got;
            do { got = ::pread(fd_, header, sizeof(header), 0); } while (got < 0 && errno == EINTR);
#endif
            if (got == sizeof(header) && !std::memcmp(header + 4, "ftyp", 4)) {
                // MOV can otherwise seek past mdat to inspect trailing atoms before playing.
                format->flags |= AVFMT_FLAG_IGNIDX;
                clear_ignore_index_ = true;
                opening_mp4_ = true;
                state_->beginOpen();
            }
        }
        if (readahead) {
            ahead_.reset(new DownloadReadAhead);
            if (!ahead_->open(state_, diskRead, this)) ahead_.reset();
        }
        return 1;
    }
    void finishOpen(AVFormatContext* format) {
        if (opening_mp4_) { state_->endOpen(); opening_mp4_ = false; }
        if (clear_ignore_index_ && format) format->flags &= ~AVFMT_FLAG_IGNIDX;
        clear_ignore_index_ = false;
    }
    void release() {
        if (opening_mp4_) { state_->endOpen(); opening_mp4_ = false; }
        ahead_.reset(); // Join before closing the descriptor or releasing the stream.
        stream_diagnostics::remove(this);
        if (io_) { av_freep(&io_->buffer); avio_context_free(&io_); }
        if (fd_ >= 0) {
#if defined(__vita__) || defined(VITAGRAM_TEST_SCEIO)
            sceIoClose(fd_);
#else
            ::close(fd_);
#endif
        }
        fd_ = -1; position_ = 0; complete_size_ = 0; resume_bytes_ = 0; clear_ignore_index_ = false; state_.reset(); video_waiting_ = nullptr;
        if (pinned_) { --download_stream::readers; pinned_ = false; }
    }
private:
    static int diskRead(void* opaque, uint8_t* buffer, int count, int64_t position) {
        auto& self = *static_cast<DownloadAvio*>(opaque);
#if defined(__vita__) || defined(VITAGRAM_TEST_SCEIO)
        int got;
        do { got = sceIoPread(self.fd_, buffer, count, position); }
        while (static_cast<uint32_t>(got) == 0x80010004u);
#else
        ssize_t got;
        do { got = ::pread(self.fd_, buffer, count, position); } while (got < 0 && errno == EINTR);
#endif
        return got < 0 ? AVERROR(EIO) : static_cast<int>(got);
    }
    static int read(void* opaque, uint8_t* buffer, int requested) {
        auto& self = *static_cast<DownloadAvio*>(opaque);
        auto progress_at = std::chrono::steady_clock::now();
        struct Wait {
            std::atomic<bool>* video_waiting;
            std::chrono::steady_clock::time_point started{};
            bool active = false;
            ~Wait() {
                if (video_waiting) *video_waiting = false;
                if (active) performance::observe(performance::Metric::StreamWait,
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
            }
        } waiting{self.video_waiting_};
        int64_t last_prefix = -1, last_tail_end = -1;
        bool last_tail_active = false;
        for (;;) {
            if (self.interrupt_.callback && self.interrupt_.callback(self.interrupt_.opaque)) return AVERROR_EXIT;
            auto s = self.state_->snapshot();
            if (self.complete_size_) {
                s.size = s.prefix = self.complete_size_; s.complete = true;
            }
            if (s.size <= 0 || (!self.complete_size_ && s.path.empty())) return AVERROR(EIO);
            if (self.position_ >= s.size) return AVERROR_EOF;
            int64_t available = s.available(self.position_);
            const int64_t reserve = std::min(self.resume_bytes_, s.size - self.position_);
            if (available > 0 && (!waiting.active || s.complete || available >= reserve)) {
                int count = static_cast<int>(std::min<int64_t>(requested, available));
                stream_diagnostics::update(&self, self.video_waiting_ != nullptr, "read", self.position_, s.prefix, s.size);
                int got;
                if (self.ahead_) got = self.ahead_->read(buffer, count, self.position_, self.interrupt_);
                else {
                    performance::Scope reading(performance::Metric::StreamRead);
                    got = diskRead(&self, buffer, count, self.position_);
                }
                stream_diagnostics::update(&self, self.video_waiting_ != nullptr, got <= 0 ? "error" : "idle",
                    self.position_ + std::max<int64_t>(0, got), s.prefix, s.size, static_cast<int>(got));
                if (got == AVERROR_EXIT) return got;
                if (got <= 0) return AVERROR(EIO); // TDLib promised these bytes were readable.
                self.position_ += got;
                return static_cast<int>(got);
            }
            if (s.complete) return AVERROR(EIO);
            if (self.opening_mp4_) self.state_->requestTail(self.position_);
            const bool tail_active = self.state_->tailActive();
            stream_diagnostics::update(&self, self.video_waiting_ != nullptr, "wait_prefix", self.position_, s.prefix, s.size,
                0, s.tail_begin, s.tail_end, tail_active);
            if (!waiting.active) { waiting.started = std::chrono::steady_clock::now(); waiting.active = true; }
            if (waiting.video_waiting && std::chrono::steady_clock::now() - waiting.started >= std::chrono::milliseconds(500))
                *waiting.video_waiting = true;
            if (s.prefix != last_prefix || s.tail_end != last_tail_end || tail_active != last_tail_active) {
                last_prefix = s.prefix; last_tail_end = s.tail_end; last_tail_active = tail_active;
                progress_at = std::chrono::steady_clock::now();
            }
            const auto timeout = std::chrono::microseconds(tail_active ? download_stream::tail_timeout_us : 60000000);
            if (std::chrono::steady_clock::now() - progress_at > timeout) return AVERROR(ETIMEDOUT);
            std::unique_lock<std::mutex> lock(self.state_->mutex);
            self.state_->changed.wait_for(lock, std::chrono::milliseconds(20));
        }
    }
    static int64_t seek(void* opaque, int64_t offset, int whence) {
        auto& self = *static_cast<DownloadAvio*>(opaque);
        const int64_t size = self.complete_size_ ? self.complete_size_ : self.state_->snapshot().size;
        if (whence & AVSEEK_SIZE) return size;
        whence &= ~AVSEEK_FORCE;
        int64_t base;
        if (whence == SEEK_SET) base = 0;
        else if (whence == SEEK_CUR) base = self.position_;
        else if (whence == SEEK_END) base = size;
        else return AVERROR(EINVAL);
        if (offset < -base || offset > INT64_MAX - base || base + offset > size) return AVERROR(EINVAL);
        self.position_ = base + offset;
        return self.position_;
    }
    std::unique_ptr<DownloadReadAhead> ahead_;
    download_stream::Shared state_;
    AVIOInterruptCB interrupt_{};
    AVIOContext* io_ = nullptr;
    int fd_ = -1;
    int64_t position_ = 0, complete_size_ = 0, resume_bytes_ = 0;
    bool pinned_ = false, clear_ignore_index_ = false, opening_mp4_ = false;
    std::atomic<bool>* video_waiting_ = nullptr;
};
