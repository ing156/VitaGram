#pragma once
#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <string>

class AudioRecorder {
public:
    ~AudioRecorder();
    bool start(const std::string& path);
    std::string stop();
    void cancel();
    bool recording() const { return recording_.load(); }
    int seconds() const;
    int level() const { return level_.load(); }
    int peak() const { return peak_.load(); }
    std::string status() const;

private:
    static void* entry(void* self);
    void run();
    bool encodeOggOpus();

    pthread_t thread_{};
    std::atomic<bool> stop_{false};
    std::atomic<bool> recording_{false};
    std::atomic<int> level_{0};
    std::atomic<int> peak_{0};
    mutable pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::string path_;
    std::string pcm_path_;
    std::string status_;
    uint32_t data_bytes_ = 0;
    bool started_ = false;
};
