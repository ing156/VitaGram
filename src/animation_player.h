#pragma once
#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <string>
#include <vector>
#include <vita2d.h>
#include "voice_player.h"

// One decoder, one queued RGBA frame. Worker never touches GPU resources.
class AnimationPlayer {
public:
    ~AnimationPlayer();
    void setSource(const std::string& path, bool video = false, bool round = false); // UI, outside drawing
    void toggle(); // Video play/pause/replay; GIFs keep looping automatically.
    int seconds() const { return position_us_.load() / 1000000; }
    void upload();                         // UI, outside drawing
    vita2d_texture* texture() const { return texture_; }
    std::string status();
    void shutdown();
private:
    pthread_t thread_{};
    bool started_ = false;
    bool video_ = false, round_ = false;
    VoicePlayer audio_;
    std::atomic<bool> paused_{false}, finished_{false};
    std::atomic<int64_t> position_us_{0};
    std::atomic<bool> stopping_{false};
    std::atomic<unsigned> generation_{0};
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::string source_, status_;
    std::vector<unsigned char> pixels_;
    int width_ = 0, height_ = 0;
    bool ready_ = false;
    vita2d_texture* texture_ = nullptr;
    struct Job { AnimationPlayer* player; unsigned generation; };
    static void* entry(void* self);
    static int interrupted(void* job);
    void run();
    void decode(const std::string& path, unsigned generation, bool video, bool round);
    void decodeTgs(const std::string& path, unsigned generation);
    void setStatus(unsigned generation, const std::string& text);
};
