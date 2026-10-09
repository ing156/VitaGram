#pragma once
#include "vault.h"
#include <pthread.h>
#include <deque>
#include <memory>
#include <vector>

namespace vault {
// Reads a sealed file ahead of the consumer on a helper thread, so the thread that decodes video
// never waits for the card or for decryption. Sequential reads are served from a ring of
// decrypted 64 KiB blocks; a seek inside the buffered range just drops blocks, any other seek
// restarts the helper at the new position.
class ReadAhead {
public:
    ~ReadAhead() { close(); }
    // The helper thread may be registered with a CPU scheduler through these hooks (the returned
    // pointer is passed back to `leave` when the thread ends).
    static void set_thread_hooks(void* (*enter)(), void (*leave)(void*));
    bool open(std::unique_ptr<Reader> reader);
    void close();
    int64_t size() const { return size_; }
    // Bytes read, 0 at the end, -1 on error. Returns as soon as some data is available.
    int64_t read(void* buffer, size_t length);
    // fseek semantics; returns the new offset or -1.
    int64_t seek(int64_t offset, int whence);
    // Reader counters; they stay readable after close().
    struct Stats { int64_t bytes = 0, micros = 0, io_micros = 0; };
    Stats stats() const;
private:
    struct Block { std::vector<uint8_t> data; size_t used = 0; };
    static void* entry(void* self);
    void run();
    static constexpr size_t kBlock = 65536, kBlocks = 8;
    std::unique_ptr<Reader> reader_;
    pthread_t thread_{};
    bool started_ = false;
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::deque<Block> ring_;   // decrypted data for [pos_, fill_pos_)
    int64_t size_ = 0, pos_ = 0, fill_pos_ = 0, seek_target_ = -1;
    bool stop_ = false, eof_ = false, failed_ = false;
    Stats final_stats_;
};
}
