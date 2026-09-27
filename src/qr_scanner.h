#pragma once
#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <string>
#include <vector>
struct quirc;
struct vita2d_texture;

// Main-thread lifecycle/upload; camera capture and QR decoding run on a worker.
class QrScanner {
public:
    QrScanner();
    ~QrScanner();
    QrScanner(const QrScanner&) = delete;
    QrScanner& operator=(const QrScanner&) = delete;
    bool start();
    void stop();
    bool active() const { return started_; }
    void upload(); // Before vita2d_start_drawing only.
    vita2d_texture* preview() const { return texture_; }
    std::string takeLink();
    std::string status();
private:
    static void* entry(void* self);
    void run();
    bool fail(const char* operation, int code);
    bool started_ = false, camera_open_ = false, camera_started_ = false, thread_started_ = false;
    int memory_ = -1;
    unsigned char* camera_buffer_ = nullptr;
    quirc* decoder_ = nullptr;
    vita2d_texture* texture_ = nullptr;
    pthread_t thread_{};
    pthread_mutex_t mutex_{};
    std::atomic<bool> stopping_{false};
    std::vector<uint32_t> preview_pixels_;
    bool preview_ready_ = false;
    std::string link_, status_;
};
