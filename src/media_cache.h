#pragma once
#include <jansson.h>
#include <pthread.h>
#include <cstdint>
#include <string>

class MediaCache {
public:
    ~MediaCache();
    void load(); // Before starting workers.
    void resolve(json_t* root); // UI: decorate received file snapshots only.
    void flush(); // TD worker only; disk I/O stays off the rendering thread.
    std::string status();
private:
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    json_t* index_ = nullptr;
    uint64_t revision_ = 0, saved_ = 0;
    uint64_t hits_ = 0;
    void walk(json_t* root, unsigned depth);
};
