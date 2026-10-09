#pragma once
#include "vault.h"
#include "vault_readahead.h"
#include <memory>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

// Lets FFmpeg demux a sealed file: decryption happens chunk by chunk behind a custom AVIOContext,
// so nothing is ever written to disk in plain text and seeking stays cheap. Larger files are read
// and decrypted ahead of the demuxer on a helper thread (vault::ReadAhead), so the thread that
// decodes never waits for the card. Plain files are left to FFmpeg's own file protocol (no
// behaviour or speed change for unencrypted installs).
class VaultAvio {
public:
    VaultAvio() = default;
    ~VaultAvio() { release(); }
    VaultAvio(const VaultAvio&) = delete;
    VaultAvio& operator=(const VaultAvio&) = delete;
    // 1: attached to `format` (open it with avformat_open_input and an empty/any URL);
    // 0: the file is not sealed, open it normally; <0: could not be opened (locked vault, damaged file).
    int attach(AVFormatContext* format, const std::string& path);
    // Call after avformat_close_input().
    void release();
private:
    static int read(void* opaque, uint8_t* buffer, int size);
    static int64_t seek(void* opaque, int64_t offset, int whence);
    std::unique_ptr<vault::Reader> reader_;      // direct path (small files)
    std::unique_ptr<vault::ReadAhead> ahead_;    // read-ahead path (owns its reader)
    const vault::Reader* stats_ = nullptr;
    int64_t size_ = 0;
    AVIOContext* io_ = nullptr;
};
