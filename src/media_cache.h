#pragma once
#include <jansson.h>
#include <pthread.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class MediaCache {
public:
    ~MediaCache();
    void clear();
    void load(); // Disk worker; publish loaded metadata under a short lock.
    // verify may do I/O; decorate=false leaves protocol payload untouched for uploads.
    bool resolve(json_t* root, bool verify = true, bool (*proceed)(void*) = nullptr, void* context = nullptr, bool decorate = true);
    void invalidate(); // After storage cleanup; never remove media files here.
    // UI queues independent file copies; no filesystem calls or protocol replay.
    std::vector<int32_t> enqueue(json_t* root); // Displaced IDs may be requested again by visible UI.
    void prioritize(int32_t file_id); // UI, RAM only: visible files before background checks.
    // UI, RAM only: a cached copy of this file is still being verified. TDLib
    // has no file database here, so downloading now would store a duplicate.
    bool awaiting(int32_t file_id);
    bool validateNext(bool (*proceed)(void*) = nullptr, void* context = nullptr); // Storage worker.
    json_t* takeValidated(); // UI owns result; only pass to updateFile, never process.
    void flush(bool (*proceed)(void*) = nullptr, void* context = nullptr); // Cache worker only.
    std::string status();
private:
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    // Diagnostics: who holds the mutex, so a 50+ ms wait or hold names its owner.
    std::atomic<const char*> holder_{nullptr};
    std::atomic<int> holder_line_{0};
    int64_t held_since_ = 0;
    void lockAs(const char* who, int line);
    void unlockHeld();
    json_t* index_ = nullptr;
    uint64_t revision_ = 0, saved_ = 0;
    uint64_t hits_ = 0;
    uint64_t generation_ = 0;
    struct Validation { bool valid; int64_t checked; };
    std::unordered_map<std::string, Validation> validated_;
    struct Pending { json_t* file; uint64_t serial; int64_t queued = 0; bool working = false, ready = false, priority = false; };
    static constexpr size_t pending_limit = 512;
    std::unordered_map<int32_t, Pending> pending_;
    uint64_t next_serial_ = 0;
    unsigned priority_turn_ = 0, ready_turn_ = 0;
    void enqueueFiles(json_t* root, unsigned depth, std::vector<int32_t>& displaced); // mutex held
    void clearPending(); // mutex held
    void candidates(json_t* root, unsigned depth, std::vector<std::pair<std::string, int64_t>>& paths);
    bool checked(const char* path, int64_t size) const;
    void walk(json_t* root, unsigned depth);
};
