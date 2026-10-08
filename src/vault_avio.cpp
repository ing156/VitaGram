#include "vault_avio.h"
#include <cstdio>

int VaultAvio::read(void* opaque, uint8_t* buffer, int size) {
    auto* self = static_cast<VaultAvio*>(opaque);
    int64_t n = self->reader_->read(buffer, (size_t)size);
    return n > 0 ? (int)n : n == 0 ? AVERROR_EOF : AVERROR(EIO);
}

int64_t VaultAvio::seek(void* opaque, int64_t offset, int whence) {
    auto* self = static_cast<VaultAvio*>(opaque);
    if (whence & AVSEEK_SIZE) return self->reader_->size();
    whence &= ~AVSEEK_FORCE;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return AVERROR(EINVAL);
    int64_t at = self->reader_->seek(offset, whence);
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
    reader_ = std::move(reader);
    io_ = avio_alloc_context(buffer, kBuffer, 0, this, read, nullptr, seek);
    if (!io_) { av_free(buffer); reader_.reset(); return -1; }
    format->pb = io_;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    return 1;
}

void VaultAvio::release() {
    if (reader_ && reader_->stat_bytes() > 0) {
        // One line per playback: how much the card read + decryption cost. Shows whether
        // decryption keeps up with the video (MB/s well above the video bit rate).
        if (FILE* log = std::fopen("ux0:data/vitagram/vault-playback.log", "a")) {
            double seconds = reader_->stat_micros() / 1e6;
            std::fprintf(log, "read=%lld KiB in %.2f s (%.1f MiB/s), card wait %.2f s, decrypt %.2f s\n",
                         (long long)(reader_->stat_bytes() / 1024), seconds,
                         seconds > 0 ? reader_->stat_bytes() / 1048576.0 / seconds : 0.0,
                         reader_->stat_io_micros() / 1e6, (reader_->stat_micros() - reader_->stat_io_micros()) / 1e6);
            std::fclose(log);
        }
    }
    if (io_) { av_freep(&io_->buffer); avio_context_free(&io_); }
    io_ = nullptr;
    reader_.reset();
}
