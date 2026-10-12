#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <pthread.h>
#include <string>

// Checks GitHub Releases for a newer signed VPK, downloads and verifies it, then installs it
// through the system package promoter. Work runs on one background thread; the UI polls snapshot().
class Updater {
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Verifying, Extracting, Installing, Installed, Failed };
    enum class Error { None, Network, NoRelease, BadSignature, Storage, Package, Install, Cancelled };
    struct Snapshot {
        State state = State::Idle;
        Error error = Error::None;
        std::string version;   // latest release (Available and later)
        std::string detail;    // technical reason for Failed, English, for the diagnostics line
        int percent = 0;       // Downloading / Extracting
        uint64_t bytes = 0, total = 0;
        bool busy() const { return state == State::Checking || state == State::Downloading || state == State::Verifying ||
                                   state == State::Extracting || state == State::Installing; }
    };

    Updater(std::string current_version, std::string ca_file, std::string work_dir);
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    void check();     // Idle/UpToDate/Failed -> Checking -> UpToDate | Available | Failed
    void install();   // Available -> Downloading -> ... -> Installed | Failed
    void cancel();
    // Installing needs a lot of free memory (0x8001000C = ENOMEM while the whole app is running), so
    // the downloaded package is only staged; the next process start installs it before anything heavy
    // is loaded. Call applyPending() first thing in main().
    enum class Pending { None, Applied, Failed };
    // Installs only a package newer than `current_version` (no rollback through a staged older release).
    static Pending applyPending(const std::string& work_dir, const std::string& current_version, std::string& detail);
    void reportStartupFailure(const std::string& detail); // result of a failed applyPending()
    Snapshot snapshot() const;
    const std::string& currentVersion() const { return current_; }

private:
    bool start(void* (*entry)(void*));
    static void* checkEntry(void* self);
    static void* installEntry(void* self);
    void runCheck();
    void runInstall();
    void set(State state);
    void fail(Error error, const std::string& detail);
    bool finishThread();
    static bool promote(const std::string& package_dir, std::string& detail);
    static bool stageHead(const std::string& package_dir, std::string& detail);

    std::string current_, ca_file_, work_dir_;
    mutable std::mutex mutex_;
    Snapshot snap_;
    std::string vpk_url_, sig_url_, vpk_name_;
    uint64_t vpk_size_ = 0;
    std::atomic<bool> cancel_{false};
    pthread_t thread_{};
    bool thread_started_ = false;
};
