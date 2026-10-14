#pragma once
#include "i18n.h"
#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <memory>
#include <string>
#include <vector>
struct vita2d_texture;
namespace video_note { class CaptureBuffer; }

class VideoNoteRecorder {
public:
    enum class State { Closed, Opening, Preview, Recording, Encoding, Review, Error };
    VideoNoteRecorder();
    ~VideoNoteRecorder();
    bool open(const std::string& path);
    void record();
    void finish();
    void switchCamera();
    void cancel();
    // UI relinquishes ownership only after Telegram accepts the send request locally.
    void releaseFile();
    State state() const { return state_.load(); }
    bool active() const { return state() != State::Closed; }
    int seconds() const;
    int percent() const { return progress_.load(); }
    int level() const { return level_.load(); }
    bool rearCamera() const { return device_.load() == 1; }
    i18n::Text status() const;
    const std::string& path() const { return path_; }
    void upload();
    vita2d_texture* preview() const { return texture_; }
private:
    static void* entry(void*);
    static void* audioEntry(void*);
    static void* writerEntry(void*);
    void run();
    void captureAudio();
    void writeCapture();
    bool openCamera(int device);
    void closeCamera();
    void setStatus(const i18n::Text& value);
    void fail(const i18n::Text& value);
    pthread_t thread_{}, audio_thread_{}, writer_thread_{};
    bool thread_started_ = false, audio_started_ = false, writer_started_ = false;
    std::unique_ptr<video_note::CaptureBuffer> capture_;
    std::atomic<bool> writer_error_{false};
    bool camera_open_ = false, camera_started_ = false;
    int memory_ = -1;
    uint8_t* camera_ = nullptr;
    vita2d_texture* texture_ = nullptr;
    std::atomic<State> state_{State::Closed};
    std::atomic<bool> cancel_{false}, begin_{false}, finish_{false}, switch_{false};
    std::atomic<bool> audio_stop_{false}, audio_done_{false}, audio_error_{false};
    std::atomic<uint32_t> samples_{0};
    std::atomic<int> device_{0}, progress_{0}, level_{0};
    mutable pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::vector<uint32_t> preview_pixels_;
    bool preview_ready_ = false;
    std::string path_, raw_path_, pcm_path_;
    i18n::Text status_;
};
