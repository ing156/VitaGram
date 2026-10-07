#pragma once
#include <jansson.h>
#include <pthread.h>
#include <cstdint>
#include <unordered_set>
class UiCache {
public:
    UiCache();
    ~UiCache();
    void load(bool (*proceed)(void*) = nullptr, void* context = nullptr); // Storage; cooperative read/parse/merge.
    void remember(json_t* update, uint64_t wire_bytes = 0);
    // Parser thread: persists a `messages` reply before the UI sees it, copying outside the mutex.
    void rememberMessages(json_t* root, uint64_t wire_bytes = 0);
    json_t* snapshot(); // New reference, caller owns it.
    json_t* startupSnapshot(); // Metadata, names and separate local small-avatar descriptors; no history.
    json_t* loadStartup(); // Storage; independent small index, nullptr for legacy cache.
    void saveStartup(json_t* snapshot, bool (*proceed)(void*) = nullptr, void* context = nullptr);
    json_t* stickerSnapshot(); // On demand, not part of startup.
    json_t* chatAvatar(int64_t id); // One small file, Storage only.
    void saveStickerPacks(json_t* value);
    void saveStickerItems(int64_t set_id, json_t* value);
    json_t* stickerItems(int64_t set_id);
    json_t* chats(); // New reference, caller owns it.
    json_t* users();
    json_t* folders();
    json_t* history(int64_t id);
    void clear();
    void flush(bool (*proceed)(void*) = nullptr, void* context = nullptr); // Worker only.
private:
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    json_t* data_ = nullptr;
    json_t* startup_avatars_ = nullptr; // Independent local file metadata, guarded by mutex_.
    uint64_t revision_ = 0, saved_ = 0;
    uint64_t generation_ = 0;
    bool loaded_ = false;
    std::unordered_set<int64_t> early_history_;
    json_t* frozenSnapshot(); // mutex held; copy containers, retain immutable leaves.
};
