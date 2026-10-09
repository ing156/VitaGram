#include "vault_avio.h"
#include "cpu_scheduler.h"
#include <cstdio>
#include <cstdio>

int VaultAvio::read(void* opaque, uint8_t* buffer, int size) {
    auto* self = static_cast<VaultAvio*>(opaque);
    int64_t n = self->ahead_ ? self->ahead_->read(buffer, (size_t)size) : self->reader_->read(buffer, (size_t)size);
    return n > 0 ? (int)n : n == 0 ? AVERROR_EOF : AVERROR(EIO);
}

int64_t VaultAvio::seek(void* opaque, int64_t offset, int whence) {
    auto* self = static_cast<VaultAvio*>(opaque);
    if (whence & AVSEEK_SIZE) return self->size_;
    whence &= ~AVSEEK_FORCE;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return AVERROR(EINVAL);
    int64_t at = self->ahead_ ? self->ahead_->seek(offset, whence) : self->reader_->seek(offset, whence);
    return at < 0 ? AVERROR(EIO) : at;
}

int VaultAvio::attach(AVFormatContext* format, const std::string& path) {
    release();
    auto reader = std::make_unique<vault::Reader>();
    if (!reader->open(path)) return vault::is_sealed(path) ? -1 : 0;
    if (!reader->sealed()) return 0;
    constexpr int kBuffer = 64 * 1024;
    auto* buffer = static_cast<uint8_t*>(av_malloc(kBuffer));
    if (!buffer) return -1;
    size_ = reader->size();
    // Files of a few hundred KiB (voice messages) are cheaper to read directly than to feed a thread.
    if (size_ >= 512 * 1024) {
        static bool hooked = false;
        if (!hooked) {
            hooked = true;
            vault::ReadAhead::set_thread_hooks(
                []() -> void* { return new cpu_scheduler::Thread(cpu_scheduler::Role::Prefetch); },
                [](void* token) { delete static_cast<cpu_scheduler::Thread*>(token); });
        }
        auto ahead = std::make_unique<vault::ReadAhead>();
        stats_ = reader.get();
        if (ahead->open(std::move(reader))) ahead_ = std::move(ahead);
        else { av_free(buffer); stats_ = nullptr; return -1; }
    } else {
        stats_ = reader.get();
        reader_ = std::move(reader);
    }
    io_ = avio_alloc_context(buffer, kBuffer, 0, this, read, nullptr, seek);
    if (!io_) { av_free(buffer); ahead_.reset(); reader_.reset(); stats_ = nullptr; return -1; }
    format->pb = io_;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    return 1;
}

void VaultAvio::release() {
    if (ahead_) ahead_->close(); // joins the helper
    vault::ReadAhead::Stats stats;
    if (ahead_) stats = ahead_->stats();
    else if (reader_) stats = {reader_->stat_bytes(), reader_->stat_micros(), reader_->stat_io_micros()};
    if (stats.bytes > 0) {
        // One line per playback: card + decryption cost, so slow playback can be told apart from
        // a slow card (card wait) or slow decryption (decrypt). With read-ahead these run on the
        // helper thread, so the numbers are not time the decoder waited.
        if (FILE* log = std::fopen("ux0:data/vitagram/vault-playback.log", "a")) {
            double seconds = stats.micros / 1e6;
            std::fprintf(log, "read=%lld KiB in %.2f s (%.1f MiB/s), card wait %.2f s, decrypt %.2f s%s\n",
                         (long long)(stats.bytes / 1024), seconds, seconds > 0 ? stats.bytes / 1048576.0 / seconds : 0.0,
                         stats.io_micros / 1e6, (stats.micros - stats.io_micros) / 1e6, ahead_ ? " [read-ahead]" : "");
            std::fclose(log);
        }
    }
    if (io_) { av_freep(&io_->buffer); avio_context_free(&io_); }
    io_ = nullptr;
    reader_.reset(); ahead_.reset(); stats_ = nullptr;
}
