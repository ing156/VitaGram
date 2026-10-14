#pragma once
#include "download_stream.h"
#include <chrono>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <unistd.h>
extern "C" {
#include <libavformat/avformat.h>
}

// Bounded disk reads of a growing file. An unavailable byte is a wait, never EOF.
class DownloadAvio {
public:
    DownloadAvio() = default;
    DownloadAvio(const DownloadAvio&) = delete;
    DownloadAvio& operator=(const DownloadAvio&) = delete;
    ~DownloadAvio() { release(); }
    int attach(AVFormatContext* format, download_stream::Shared state) {
        release();
        state_ = std::move(state);
        interrupt_ = format->interrupt_callback;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        for (;;) {
            if (interrupt_.callback && interrupt_.callback(interrupt_.opaque)) { release(); return AVERROR_EXIT; }
            auto s = state_->snapshot();
            if (s.size <= 0) { release(); return AVERROR(EIO); }
            fd_ = ::open(s.path.c_str(), O_RDONLY);
            if (fd_ >= 0) break;
            // TDLib can rename the temporary path just before its completion update arrives.
            if (s.complete || std::chrono::steady_clock::now() >= deadline) { release(); return AVERROR(EIO); }
            std::unique_lock<std::mutex> lock(state_->mutex);
            state_->changed.wait_for(lock, std::chrono::milliseconds(20));
        }
        constexpr int buffer_size = 64 * 1024;
        auto* buffer = static_cast<uint8_t*>(av_malloc(buffer_size));
        if (!buffer) { release(); return AVERROR(ENOMEM); }
        io_ = avio_alloc_context(buffer, buffer_size, 0, this, read, nullptr, seek);
        if (!io_) { av_free(buffer); release(); return AVERROR(ENOMEM); }
        ++download_stream::readers; pinned_ = true;
        format->pb = io_;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        return 1;
    }
    void release() {
        if (io_) { av_freep(&io_->buffer); avio_context_free(&io_); }
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1; position_ = 0; state_.reset();
        if (pinned_) { --download_stream::readers; pinned_ = false; }
    }
private:
    static int read(void* opaque, uint8_t* buffer, int requested) {
        auto& self = *static_cast<DownloadAvio*>(opaque);
        auto progress_at = std::chrono::steady_clock::now();
        int64_t last_prefix = -1;
        for (;;) {
            if (self.interrupt_.callback && self.interrupt_.callback(self.interrupt_.opaque)) return AVERROR_EXIT;
            auto s = self.state_->snapshot();
            if (s.size <= 0 || s.path.empty()) return AVERROR(EIO);
            if (self.position_ >= s.size) return AVERROR_EOF;
            int64_t available = std::min(s.size, s.prefix) - self.position_;
            if (available > 0) {
                int count = static_cast<int>(std::min<int64_t>(requested, available));
                if (::lseek(self.fd_, self.position_, SEEK_SET) < 0) return AVERROR(EIO);
                auto got = ::read(self.fd_, buffer, count);
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) return AVERROR(EIO); // TDLib promised these bytes were readable.
                self.position_ += got;
                return static_cast<int>(got);
            }
            if (s.complete) return AVERROR(EIO);
            if (s.prefix != last_prefix) { last_prefix = s.prefix; progress_at = std::chrono::steady_clock::now(); }
            if (std::chrono::steady_clock::now() - progress_at > std::chrono::seconds(60)) return AVERROR(ETIMEDOUT);
            std::unique_lock<std::mutex> lock(self.state_->mutex);
            self.state_->changed.wait_for(lock, std::chrono::milliseconds(20));
        }
    }
    static int64_t seek(void* opaque, int64_t offset, int whence) {
        auto& self = *static_cast<DownloadAvio*>(opaque);
        const int64_t size = self.state_->snapshot().size;
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
    download_stream::Shared state_;
    AVIOInterruptCB interrupt_{};
    AVIOContext* io_ = nullptr;
    int fd_ = -1;
    int64_t position_ = 0;
    bool pinned_ = false;
};
