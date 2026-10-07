#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <string>
#include <vector>
#include <vita2d.h>
#include "voice_player.h"
struct AVFrame;
struct AVCodecContext;

// One decoder with bounded hardware frame storage. Worker never draws.
class AnimationPlayer {
public:
    ~AnimationPlayer();
    void setSource(const std::string& path, bool video = false, bool round = false,
                   bool prepare = false); // UI, outside drawing
    void toggle(); // Video play/pause/replay; GIFs keep looping automatically.
    int seconds() const { return position_us_.load() / 1000000; }
    void upload();                         // UI, outside drawing
    vita2d_texture* texture() { return yuv_valid_ ? &yuv_texture_ : texture_; }
    std::string status();
    void shutdown();
private:
    pthread_t thread_{};
    bool started_ = false;
    bool video_ = false, round_ = false, prepare_ = false;
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
    AVFrame* pending_yuv_frame_ = nullptr;
    AVFrame* displayed_yuv_frame_ = nullptr;
    bool pending_direct_ = false;
    bool yuv_direct_ = false;
    struct YuvSurface {
        SceUID uid = -1;
        void* pixels = nullptr;
        unsigned allocated = 0;
        std::atomic<bool> busy{false};
    };
    // The decoder's reference frames, PTS reorder queue and UI frames coexist.
    std::array<YuvSurface, 28> yuv_surfaces_{};
    vita2d_texture yuv_texture_{};
    SceUID yuv_memblock_ = -1;
    unsigned yuv_memblock_size_ = 0;
    void* yuv_pixels_ = nullptr;
    bool yuv_mapped_ = false;
    int yuv_pitch_ = 0, yuv_height_ = 0;
    bool yuv_valid_ = false;
    bool yuv_logged_ = false;
    std::atomic<unsigned> uploaded_frames_{0};
    struct Job { AnimationPlayer* player; unsigned generation; };
    static void* entry(void* self);
    static int interrupted(void* job);
    void run();
    void decode(const std::string& path, unsigned generation, bool video, bool round, bool prepare);
    void decodeTgs(const std::string& path, unsigned generation);
    void setStatus(unsigned generation, const std::string& text);
    void releaseYuvTexture();
    static int directBuffer(AVCodecContext* codec, AVFrame* frame, int flags);
    static void releaseDirectBuffer(void* opaque, uint8_t* data);
};
