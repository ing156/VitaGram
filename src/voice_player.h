#pragma once
#include "i18n.h"
#include <atomic>
#include <algorithm>
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
    void setPlaying(bool playing);
    bool playing() const { return playing_ && !finished_; }
    // Restarts decoding of the current source at `us` (clamped by the caller). Keeps the play/pause state.
    void seekTo(int64_t us);
    int64_t seekLimitUs(int64_t duration);
    void setVolume(int percent) { volume_ = std::max(0, std::min(100, percent)); }
    int volume() const { return volume_; }
    void setSpeed(int percent);
    int speed() const { return speed_; }
    // Output position; accurate to a PCM block, includes the seek offset for voice notes and music.
    double positionSeconds() const { return samples_.load() / 48000.0; }
    bool finished() const { return finished_; }
    bool clockReady() const { return clock_ready_; }
    std::atomic<bool>& videoBuffering() { return video_buffering_; }
    std::atomic<bool>& videoFrameStalled() { return video_frame_stalled_; }
    std::atomic<bool>& videoFrameLagging() { return video_frame_lagging_; }
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
    std::atomic<bool> video_buffering_{false}, video_frame_stalled_{false}, video_frame_lagging_{false};
    std::atomic<int> volume_{100}, speed_{100};
};
