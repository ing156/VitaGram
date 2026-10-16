#pragma once
#include <jansson.h>
#include <pthread.h>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
class UiCache {
public:
    static constexpr size_t kStartupRowsPerList = 100; // Startup index keeps the top of each chat list.
    // Chat histories copied and written by one flush: bounds the copies held at once after a mass update.
    static constexpr size_t kHistoriesPerFlush = 16;
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
    // Storage: the index from the chat rows the UI confirmed (TDLib chat JSON, keyed by id). False until loaded.
    // Skipped when the cache was cleared (logout) after `generation` was taken.
    bool saveStartupList(json_t* chats, uint64_t generation, bool (*proceed)(void*) = nullptr, void* context = nullptr);
    uint64_t generation();
    json_t* stickerSnapshot(); // On demand, not part of startup.
    json_t* chatAvatar(int64_t id); // One small file, Storage only.
    void saveStickerPacks(json_t* value);
    void saveStickerItems(int64_t set_id, json_t* value);
    json_t* stickerItems(int64_t set_id);
    json_t* chats(); // New reference, caller owns it.
    json_t* users();
    json_t* folders();
    // Copies at most `newest` messages (highest IDs). References are taken under the mutex and the
    // deep copy (~1.7 ms/KB) runs without it; the UI thread asks only for one screen.
    json_t* history(int64_t id, size_t newest = SIZE_MAX);
    // Storage: chat histories live in one file per chat (ui-history/<id>.json) so that startup parses
    // only chats and names. Merges one pending file (`preferred` first); false when none is left.
    bool loadHistory(int64_t preferred = 0, bool (*proceed)(void*) = nullptr, void* context = nullptr);
    bool historyPending(int64_t id); // Its file exists but is not merged yet.
    static void removeHistoryFiles(); // Untrusted caches after a vault signature failure.
    void clear();
    // Worker only. True when histories were left for another pass by kHistoriesPerFlush.
    bool flush(bool (*proceed)(void*) = nullptr, void* context = nullptr);
private:
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    json_t* data_ = nullptr;
    json_t* startup_avatars_ = nullptr; // Independent local file metadata, guarded by mutex_.
    uint64_t revision_ = 0, saved_ = 0;
    uint64_t generation_ = 0;
    bool loaded_ = false;
    std::unordered_set<int64_t> early_history_; // Legacy monolithic cache: chats changed before its merge.
    // Per-chat history files. All guarded by mutex_.
    std::unordered_set<int64_t> history_dirty_;   // Written (or removed) by the next flush.
    std::unordered_set<int64_t> history_pending_; // On disk, not merged into memory yet.
    // Deleted live before the chat's file was merged, so an old copy cannot resurrect them.
    std::unordered_map<int64_t, std::unordered_set<int64_t>> history_deleted_;
    bool history_listed_ = false; // The history directory was read after the main cache merge.
    bool history_wipe_ = false;   // clear(): the next flush removes every history file.
    uint64_t main_revision_ = 0, main_saved_ = 0; // Chats, users, folders and stickers only.
    void touchHistory(int64_t chat); // mutex held
    json_t* frozenSnapshot(); // mutex held; copy containers, retain immutable leaves.
};
