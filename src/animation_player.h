#pragma once
#include "i18n.h"
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
                   bool prepare = false, bool cache_only = false,
                   download_stream::Shared stream = nullptr, int32_t file_id = 0); // UI, outside drawing
    bool preparationChecked() const { return preparation_checked_.load(); }
    bool preparationRequired() const { return preparation_required_.load(); }
    void toggle(); // Video play/pause/replay; GIFs keep looping automatically.
    void setVolume(int percent) { audio_.setVolume(percent); }
    int seconds() const { return position_us_.load() / 1000000; }
    int64_t positionUs() const { return position_us_.load(); }
    bool playing() const { return video_ && !paused_ && !finished_; }
    bool readyForPlayback() const { return playback_ready_.load(); }
    bool failedToOpen() const { return finished_ && !playback_ready_; }
    // Video only, not a prepared portrait copy. While streaming, seeking needs the container duration
    // to map the downloaded prefix to a time; seekTo keeps the target inside that prefix.
    bool canSeek() const { return video_ && playback_ready_ && started_ && !source_.empty() && !prepare_ && (!stream_ || stream_->snapshot().complete || knownDurationUs() > 0); }
    // Telegram's duration of the shown clip, used when the container does not report one
    // (fragmented MP4 and similar), so seeking while streaming still knows the timeline. UI thread.
    void setDurationHint(int64_t us) { if (us > 0) duration_hint_us_ = us; }
    // Latest position a seek can start from: the whole clip once local, the downloaded part while streaming.
    int64_t seekLimitUs() const;
    // Restarts the decoder at `us` (like a replay that starts later); play/pause state is kept.
    void seekTo(int64_t us);
    void upload();                         // UI, outside drawing
    // Video is opened but no current picture is moving: waiting for the first picture after opening or a seek,
    // or playback stalled (typically waiting for streamed bytes). The UI shows a loading indicator.
    bool buffering() const;
    vita2d_texture* texture() { return yuv_valid_ ? &yuv_texture_ : texture_; }
    i18n::Text status();
    void shutdown();
private:
    pthread_t thread_{};
    bool started_ = false;
    bool video_ = false, round_ = false, prepare_ = false, cache_only_ = false;
    std::atomic<bool> preparation_required_{false}, preparation_checked_{true};
    VoicePlayer audio_;
    std::atomic<bool> paused_{false}, finished_{false};
    std::atomic<bool> playback_ready_{false};
    std::atomic<int64_t> position_us_{0};
    std::atomic<int64_t> start_us_{0};   // decode start of the current generation
    std::atomic<int64_t> duration_us_{0};   // container duration of the current source, 0 while unknown
    std::atomic<int64_t> duration_hint_us_{0};
    int64_t knownDurationUs() const { const int64_t d = duration_us_.load(); return d > 0 ? d : duration_hint_us_.load(); }
    std::atomic<bool> awaiting_frame_{false};   // no picture of the current generation published yet
    std::atomic<int64_t> last_frame_us_{0};     // system time of the latest published picture
    std::atomic<int64_t> frame_step_us_{100000};   // container duration of the current source, 0 while unknown
    std::atomic<bool> stopping_{false};
    std::atomic<unsigned> generation_{0};
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::string source_;
    int32_t source_file_id_ = 0;
    i18n::Text status_;
    download_stream::Shared stream_;
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
    void decode(const std::string& path, unsigned generation, bool video, bool round, bool prepare, bool cache_only,
                download_stream::Shared stream);
    void decodeTgs(const std::string& path, unsigned generation);
    void setStatus(unsigned generation, const i18n::Text& text);
    void releaseYuvTexture();
    bool allocateYuvTexture(int pitch, int aligned_height);   // own CDRAM copy target, not from the decoder pool
    void copyNv12(const AVFrame* source);
    // Moves the picture on screen out of the decoder's surface pool into an own copy, so a restarting
    // decoder gets its surfaces back while the last picture stays visible.
    void detachDisplayedFrame();
    static int directBuffer(AVCodecContext* codec, AVFrame* frame, int flags);
    static void releaseDirectBuffer(void* opaque, uint8_t* data);
};
