#pragma once
#include "i18n.h"
#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <string>

class AudioRecorder {
public:
    ~AudioRecorder();
    bool start(const std::string& path);
    std::string stop();
    std::string takeCompleted();
    void cancel();
    // 检查录音是否正在运行（用于方形按钮取消操作）
    bool active() const { return recording_.load() || processing_.load() || started_; }
    bool recording() const { return recording_.load(); }
    bool processing() const { return processing_.load(); }
    int seconds() const;
    int conversionPercent() const { return conversion_percent_.load(); }
    int level() const { return level_.load(); }
    int peak() const { return peak_.load(); }
    i18n::Text status() const;
    void setStatus(const i18n::Text& status);

private:
    static void* entry(void* self);
    static void* encodeEntry(void* self);
    void run();
    void runEncode();
    bool encodeOggOpus(i18n::Text& error);

    pthread_t thread_{};
    pthread_t encode_thread_{};
    std::atomic<bool> stop_{false};
    std::atomic<bool> recording_{false};
    std::atomic<bool> processing_{false};
    std::atomic<int> conversion_percent_{0};
    std::atomic<int> level_{0};
    std::atomic<int> peak_{0};
    mutable pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::string path_;
    std::string pcm_path_;
    std::string completed_path_;
    i18n::Text status_;
    std::atomic<uint32_t> data_bytes_{0};
    std::atomic<bool> capture_failed_{false};
    std::atomic<bool> cancel_encode_{false};
    bool started_ = false;
    bool encode_started_ = false;
};
