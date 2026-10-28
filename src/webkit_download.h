#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <pthread.h>
#include <string>

class WebKitDownload {
public:
    enum class State { Idle, Checking, Available, Downloading, Ready, Failed };
    struct Snapshot {
        State state = State::Idle;
        uint64_t bytes = 0, total = 0;
        std::string error;
        bool busy() const { return state == State::Checking || state == State::Downloading; }
    };
    ~WebKitDownload();
    Snapshot snapshot() const;
    void prepare();
    void install();
    void cancel();
    static const char* modulePath();
    static bool verified();
private:
    void start(bool download);
    void run(bool download);
    void fail(const std::string& error);
    bool unpack();
    mutable std::mutex mutex_;
    Snapshot state_;
    std::string url_, signature_url_;
    std::atomic<bool> cancel_{false};
    pthread_t thread_{};
    bool thread_started_ = false, downloading_ = false;
};
