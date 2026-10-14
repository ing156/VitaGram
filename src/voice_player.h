#pragma once
#include "i18n.h"
#include <atomic>
#include <pthread.h>
#include <string>
#include <cstdint>
#include "download_stream.h"

class VoicePlayer {
public:
    ~VoicePlayer();
    void setSource(const std::string& path, bool video = false, bool music = false,
                   download_stream::Shared stream = nullptr);
    void toggle();
    void setPlaying(bool playing) { if (!finished_) playing_ = playing; }
    bool playing() const { return playing_ && !finished_; }
    // Restarts decoding of the current source at `us` (clamped by the caller). Keeps the play/pause state.
    void seekTo(int64_t us);
    // Output position; accurate to a PCM block, includes the seek offset for voice notes and music.
    double positionSeconds() const { return samples_.load() / 48000.0; }
    bool finished() const { return finished_; }
    bool clockReady() const { return clock_ready_; }
    int64_t positionUs() const { return clock_us_; }
    i18n::Text error();
    void shutdown();
    i18n::Text status();
    int seconds() const { return samples_.load() / 48000; }
private:
    static void* entry(void* self);
    void run();
    void decode(const std::string& path, unsigned generation, bool video, bool music, download_stream::Shared stream);
    void report(const i18n::Text& text);
    pthread_t thread_{};
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    bool started_ = false;
    bool video_ = false;
    bool music_ = false;
    std::string path_;
    i18n::Text status_;
    download_stream::Shared stream_;
    std::atomic<bool> stop_{false}, playing_{false}, finished_{false};
    std::atomic<unsigned> generation_{0};
    std::atomic<int64_t> samples_{0};
    std::atomic<int64_t> clock_us_{0};
    std::atomic<int64_t> start_us_{0};   // where the next decode generation starts
    std::atomic<bool> clock_ready_{false};
};
