#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <string>

// Checks GitHub Releases for a newer signed VPK, downloads and verifies it, then installs it
// through the system package promoter. Work runs on one background thread; the UI polls snapshot().
class Updater {
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Verifying, Extracting, Installing, Installed, Failed };
    enum class Error { None, Network, NoRelease, BadSignature, Storage, Package, Install, Cancelled, Helper };
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

    // `fallback_dir` is used when `work_dir` cannot be created/written or lacks free space.
    Updater(std::string current_version, std::string ca_file, std::string work_dir, std::string fallback_dir = "");
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    void check();     // Idle/UpToDate/Failed -> Checking -> UpToDate | Available | Failed
    void install();   // Available -> Downloading -> ... -> Installed | Failed
    void cancel();
    // Installing needs a lot of free memory (0x8001000C = ENOMEM while the whole app is running), so
    // the downloaded package is only staged; the next process start installs it before anything heavy
    // is loaded. Call applyPending() first thing in main().
    // Handoff: the package is verified and unpacked and the VitaGram Updater app has been started; this
    // process must exit now (the console cannot replace an application that is running).
    enum class Pending { None, Handoff, Failed };
    // Text the updater app shows while it installs (already in the user's language).
    struct HandoffText { std::string title, hint; };
    // Overall progress 0..100 of the preparation (signature check, unpacking, starting the updater app).
    using Progress = std::function<void(int)>;
    // Installs only a package newer than `current_version` (no rollback through a staged older release).
    // Looks for a staged package in `work_dir`, then in `fallback_dir`.
    static Pending applyPending(const std::string& work_dir, const std::string& fallback_dir,
                                const std::string& current_version, const HandoffText& text, const Progress& progress, std::string& detail);
    // True when a staged package waits in either directory (cheap: only looks for the marker).
    static bool hasPending(const std::string& work_dir, const std::string& fallback_dir);
    // After a successful update the updater app is no longer needed: it leaves a flag file, and the first
    // start of the new version removes the app (and its LiveArea bubble) again. Does nothing without the flag.
    static void removeUpdaterApp(const std::string& work_dir, const std::string& fallback_dir);
    // True when removeUpdaterApp() would do something (so the caller can show a message first).
    static bool updaterCleanupDue(const std::string& work_dir, const std::string& fallback_dir);
    void reportStartupFailure(const std::string& detail); // result of a failed applyPending()
    // Explicit, already-confirmed local VPK installation; never used by automatic updates.
    // The extracted package must identify VitaGram. On success the caller must exit, not restart itself.
    static bool prepareLocalInstaller(std::string& detail);
    static bool handoffConfirmedPackage(const std::string& package_dir, const HandoffText& text, std::string& detail);
    // A staged update still owns the installer after its download worker has finished.
    bool allowsLocalInstall() const;
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
    static Pending applyIn(const std::string& work_dir, const std::string& current_version, const HandoffText& text, const Progress& progress, std::string& detail);
    bool chooseDir(uint64_t package_size, std::string& dir, std::string& detail);
    static bool prepareInstaller(const std::string& work_dir, std::string& detail);
    static bool startInstaller(const std::string& work_dir, const std::string& package_dir, const HandoffText& text, std::string& detail);
    static bool stageHead(const std::string& package_dir, std::string& detail,
                          const char* source = "app0:sce_sys/package/head.bin");

    std::string current_, ca_file_, work_dir_, fallback_dir_;
    mutable std::mutex mutex_;
    Snapshot snap_;
    std::string vpk_url_, sig_url_, vpk_name_;
    uint64_t vpk_size_ = 0;
    std::atomic<bool> cancel_{false};
    pthread_t thread_{};
    bool thread_started_ = false;
};
