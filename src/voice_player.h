#pragma once
#include <atomic>
#include <pthread.h>
#include <string>
#include <cstdint>

class VoicePlayer {
public:
    ~VoicePlayer();
    void setSource(const std::string& path, bool video = false, bool music = false);
    void toggle();
    void setPlaying(bool playing) { if (!finished_) playing_ = playing; }
    bool finished() const { return finished_; }
    bool clockReady() const { return clock_ready_; }
    int64_t positionUs() const { return clock_us_; }
    std::string error();
    void shutdown();
    std::string status();
    int seconds() const { return samples_.load() / 48000; }
private:
    static void* entry(void* self);
    void run();
    void decode(const std::string& path, unsigned generation, bool video, bool music);
    void report(const std::string& text);
    pthread_t thread_{};
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    bool started_ = false;
    bool video_ = false;
    bool music_ = false;
    std::string path_, status_;
    std::atomic<bool> stop_{false}, playing_{false}, finished_{false};
    std::atomic<unsigned> generation_{0};
    std::atomic<int64_t> samples_{0};
    std::atomic<int64_t> clock_us_{0};
    std::atomic<bool> clock_ready_{false};
};
