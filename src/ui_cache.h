#pragma once
#include <jansson.h>
#include <pthread.h>
#include <cstdint>
class UiCache {
public:
    ~UiCache();
    void load();
    void remember(json_t* update);
    json_t* snapshot(); // New reference, caller owns it.
    void saveStickerPacks(json_t* value);
    void saveStickerItems(int64_t set_id, json_t* value);
    json_t* stickerItems(int64_t set_id);
    json_t* chats(); // New reference, caller owns it.
    json_t* users();
    json_t* folders();
    json_t* history(int64_t id);
    void clear();
    void flush(); // Worker only.
private:
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    json_t* data_ = nullptr;
    uint64_t revision_ = 0, saved_ = 0;
};
