#include "performance.h"
#include "cache_writer.h"
#include "ui_cache.h"
#include "priority_ceiling.h"
#include "config.h"
#include <utility>
#include <sys/stat.h>
#include <dirent.h>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <map>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdio>
#ifndef JANSSON_THREAD_SAFE_REFCOUNT
#error Cache snapshots require Jansson atomic reference counting
#endif
namespace {
// The UI waits on this mutex: Storage and Parser holders run at the UI priority meanwhile.
void lock_cache(pthread_mutex_t* mutex) { priority_ceiling::enter(); performance::lock(mutex, performance::Metric::UiLock); }
void unlock_cache(pthread_mutex_t* mutex) { pthread_mutex_unlock(mutex); priority_ceiling::leave(); }
const char* path = VITAGRAM_DB_DIR "/ui-cache.json";
const char* temp = VITAGRAM_DB_DIR "/ui-cache.json.tmp";
const char* startup_path = VITAGRAM_DB_DIR "/ui-startup.json";
const char* startup_temp = VITAGRAM_DB_DIR "/ui-startup.json.tmp";
const char* history_dir = VITAGRAM_DB_DIR "/ui-history";
constexpr size_t history_file_limit = 1024 * 1024; // 60 messages; media captions included.
std::string history_file(int64_t chat) { return std::string(history_dir) + "/" + std::to_string(chat) + ".json"; }
std::vector<int64_t> history_files() {
    std::vector<int64_t> chats;
    if (DIR* dir = opendir(history_dir)) {
        while (auto* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.size() <= 5 || name.compare(name.size() - 5, 5, ".json")) continue;
            char* end = nullptr;
            const long long chat = std::strtoll(name.c_str(), &end, 10);
            if (chat && end == name.c_str() + name.size() - 5) chats.push_back(chat);
        }
        closedir(dir);
    }
    return chats;
}
const char* type(json_t* o) { const char* s = json_string_value(json_object_get(o, "@type")); return s ? s : ""; }
int64_t number(json_t* o, const char* key) { return json_integer_value(json_object_get(o, key)); }
// `budget` bounds deletions per call so a UI-thread caller never frees
// hundreds of entries at once; the rest is trimmed by later calls.
void retain(json_t* object, size_t limit, size_t budget = SIZE_MAX) {
    performance::SlowScope slow(performance::SlowPhase::Retain,"collection");
    slow.counts(json_object_size(object),limit);
    while (json_object_size(object) > limit && budget--) {
        void* first = json_object_iter(object);
        if (!first) break;
        std::string key = json_object_iter_key(first); json_object_del(object, key.c_str());
    }
}
json_t* measured_copy(json_t* value,const char* context=nullptr) {
    performance::SlowScope slow(performance::SlowPhase::Copy,context ? context : type(value));
    return json_deep_copy(value);
}
void set_copy(json_t* object, const char* key, json_t* value) {
    if (value) json_object_set_new(object, key, measured_copy(value,key));
}
bool completed_file(json_t* file) {
    auto* local = json_object_get(file, "local");
    return json_is_true(json_object_get(local, "is_downloading_completed")) && json_string_value(json_object_get(local, "path"));
}
json_t* compact_chat(json_t* chat) {
    json_t* result = json_object();
    for (const char* key : {"@type", "id", "title", "type", "positions", "photo", "last_read_outbox_message_id"})
        set_copy(result, key, json_object_get(chat, key));
    return result;
}
json_t* read_cache(const char* filename, size_t limit, bool (*proceed)(void*) = nullptr, void* context = nullptr, bool* cancelled = nullptr) {
    auto allowed = [&] {
        if (!proceed || proceed(context)) return true;
        if (cancelled) *cancelled = true;
        return false;
    };
    if (!allowed()) return nullptr;
    performance::Scope reading(performance::Metric::CacheRead);
    struct stat info{};
    // The limit counts plain bytes; a sealed snapshot is a little larger on disk.
    if (stat(filename, &info) || info.st_size <= 0 || (uint64_t)info.st_size > (uint64_t)vault::sealed_size((int64_t)limit)) return nullptr;
    vault::Reader file; // sealed or plain; sealed snapshots need the unlocked vault
    // With the vault on and no migration pending, a cache that is not vault-sealed was put there
    // from outside: it is ignored (and rebuilt) rather than shown.
    if (!file.open(filename, vault::caches_must_be_sealed() ? vault::Require::Vault : vault::Require::Any) || file.size() <= 0 || (uint64_t)file.size() > limit) return nullptr;
    std::string bytes(static_cast<size_t>(file.size()), '\0');
    size_t size = 0;
    bool failed = false;
    while (size < bytes.size() && allowed()) {
        const auto count = file.read(&bytes[size], std::min<size_t>(65536, bytes.size()-size));
        if (count < 0) failed = true;
        if (count <= 0) break;
        size += (size_t)count;
    }
    const bool ok = size == bytes.size() && !failed;
    file.close(); reading.finish();
    if (!ok) return nullptr;
    performance::Scope parsing(performance::Metric::CacheParse);
    json_error_t error{};
    if (!proceed) return json_loadb(bytes.data(), bytes.size(), 0, &error);
    struct Reader { const std::string* bytes; size_t offset; bool (*proceed)(void*); void* context; bool* cancelled; };
    Reader reader{&bytes, 0, proceed, context, cancelled};
    auto read = [](void* buffer, size_t length, void* opaque) -> size_t {
        auto* r = static_cast<Reader*>(opaque);
        if (!r->proceed(r->context)) { if (r->cancelled) *r->cancelled = true; return 0; }
        size_t count = std::min(length, r->bytes->size()-r->offset);
        std::memcpy(buffer, r->bytes->data()+r->offset, count); r->offset += count;
        return count;
    };
    return json_load_callback(read, &reader, 0, &error);
}
std::string list_key(json_t* list) {
    std::string key = type(list);
    if (key == "chatListFolder") key += ":" + std::to_string(number(list, "chat_folder_id"));
    return key;
}
// The startup index holds only the top of each list (main, archive, every folder):
// it is read before the first frame of the chat list and applied in one step.
json_t* startup_copy(json_t* source, size_t per_list = UiCache::kStartupRowsPerList) {
    auto* result = json_pack("{s:i,s:{},s:{}}", "startup_format", 2, "chats", "users");
    json_object_set_new(result,"avatars",json_object());
    const char* key; json_t* value;
    struct Row { std::string key; json_t* chat; int64_t order; bool keep; };
    std::vector<Row> rows;
    std::map<std::string, std::vector<std::pair<int64_t, size_t>>> lists;
    json_object_foreach(json_object_get(source, "chats"), key, value) {
        int64_t order = 0;
        size_t i; json_t* position;
        json_array_foreach(json_object_get(value, "positions"), i, position) {
            const int64_t list_order = number(position, "order");
            if (!list_order) continue;
            const std::string list = list_key(json_object_get(position, "list"));
            if (list == "chatListMain") order = list_order;
            lists[list].push_back({list_order, rows.size()});
        }
        rows.push_back({key, value, order, false});
    }
    for (auto& list : lists) {
        auto& ranked = list.second;
        const size_t kept = std::min(per_list, ranked.size());
        std::partial_sort(ranked.begin(), ranked.begin()+kept, ranked.end(), [&](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : number(rows[a.second].chat, "id") > number(rows[b.second].chat, "id");
        });
        for (size_t i = 0; i < kept; ++i) rows[ranked[i].second].keep = true;
    }
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const Row& row) { return !row.keep; }), rows.end());
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.order != b.order ? a.order > b.order : number(a.chat, "id") > number(b.chat, "id");
    });
    for (const auto& row : rows) {
        auto* chat = json_object();
        // The cached photo only learns a download from chat updates (updateFile is not stored), so a
        // picture fetched during this session was missing here and the next start showed placeholders.
        // Prefer a completed cached file, then the path the chat list already shows, then the last index.
        json_t* shown = nullptr;
        auto* file=json_object_get(json_object_get(row.chat,"photo"),"small");
        if (!completed_file(file)) {
            if (const char* known = json_string_value(json_object_get(row.chat, "@avatar_path"))) {
                shown = json_pack("{s:s,s:i,s:{s:s,s:b}}", "@type", "file", "id", 0, "local", "path", known, "is_downloading_completed", 1);
                file = shown;
            } else if (auto* previous = json_object_get(json_object_get(source,"avatars"),row.key.c_str())) file = previous;
        }
        auto* local=json_object_get(file,"local");
        const char* local_path=json_string_value(json_object_get(local,"path"));
        if (json_is_true(json_object_get(local,"is_downloading_completed")) && local_path && std::strlen(local_path)<=1024) {
            auto* small=json_pack("{s:s,s:i}","@type","file","id",0);
            set_copy(small,"size",json_object_get(file,"size"));
            set_copy(small,"remote",json_object_get(file,"remote"));
            set_copy(small,"local",local);
            json_object_set_new(json_object_get(result,"avatars"),row.key.c_str(),small);
        }
        if (shown) json_decref(shown);
        for (const char* field : {"@type", "id", "title", "type", "positions", "permissions", "last_read_outbox_message_id",
                                  "unread_count", "is_marked_as_unread"})
            set_copy(chat, field, json_object_get(row.chat, field));
        if (const int64_t last = number(json_object_get(row.chat, "last_message"), "id"))
            json_object_set_new(chat, "last_message", json_pack("{s:I}", "id", (json_int_t)last));
        if (!json_is_array(json_object_get(chat, "positions"))) json_object_set_new(chat, "positions", json_array());
        json_object_set_new(json_object_get(result, "chats"), row.key.c_str(), chat);
    }
    json_object_foreach(json_object_get(source, "users"), key, value) {
        auto* user = json_object();
        for (const char* field : {"id", "first_name", "last_name", "type"}) set_copy(user, field, json_object_get(value, field));
        json_object_set_new(json_object_get(result, "users"), key, user);
    }
    set_copy(result, "folders", json_object_get(source, "folders"));
    return result;
}
json_t* freeze_tree(json_t* data) {
    json_t* copy = json_copy(data);
    if (!copy) return nullptr;
    for (const char* key : {"chats", "users", "sticker_items", "history"}) {
        json_t* source = json_object_get(data, key);
        if (!source) continue;
        json_t* collection = json_copy(source);
        if (!collection || json_object_set_new(copy, key, collection)) { json_decref(copy); return nullptr; }
        if (!std::strcmp(key, "history")) {
            const char* chat; json_t* messages;
            json_object_foreach(source, chat, messages) {
                json_t* items = json_copy(messages);
                if (!items || json_object_set_new(collection, chat, items)) { json_decref(copy); return nullptr; }
            }
        }
    }
    return copy;
}
// Dropping a whole cached chat (up to 60 messages) at once took 100-250 ms on
// the UI thread. Evict the oldest other chat a few messages per call instead;
// the collection may exceed the limit by one chat until it is empty.
// With `garbage`, removed values are kept alive there so the caller can free them after the mutex.
void shrink_histories(json_t* histories, size_t limit, const std::string& keep, std::vector<json_t*>* garbage = nullptr,
                      std::vector<int64_t>* touched = nullptr) {
    if (json_object_size(histories) <= limit) return;
    const char* victim_key = nullptr; json_t* victim = nullptr; const char* k; json_t* v;
    json_object_foreach(histories, k, v) if (keep != k) { victim_key = k; victim = v; break; }
    if (!victim) return;
    std::string victim_name = victim_key;
    if (touched) touched->push_back(std::strtoll(victim_name.c_str(), nullptr, 10));
    for (int n = 0; n < 8; ++n) {
        void* first = json_object_iter(victim);
        if (!first) break;
        std::string message = json_object_iter_key(first);
        if (garbage) garbage->push_back(json_incref(json_object_iter_value(first)));
        json_object_del(victim, message.c_str());
    }
    if (json_object_size(victim) == 0) json_object_del(histories, victim_name.c_str());
}
void put_message(json_t* histories, json_t* message, std::vector<int64_t>& touched) {
    int64_t chat = number(message, "chat_id"), id = number(message, "id");
    // Don't persist unsent messages or self-destructing content.
    if (!chat || id <= 0 || json_is_object(json_object_get(message, "sending_state")) ||
        json_is_object(json_object_get(message, "self_destruct_type")) ||
        json_is_true(json_object_get(json_object_get(message, "content"), "is_secret"))) return;
    std::string key = std::to_string(chat);
    touched.push_back(chat);
    json_t* items = json_object_get(histories, key.c_str());
    if (!items) { items = json_object(); json_object_set_new(histories, key.c_str(), items); }
    json_object_set_new(items, std::to_string(id).c_str(), measured_copy(message));
    while (json_object_size(items) > 60) {
        const char* k; json_t* v; int64_t oldest = INT64_MAX;
        json_object_foreach(items, k, v) oldest = std::min(oldest, number(v, "id"));
        json_object_del(items, std::to_string(oldest).c_str());
    }
    shrink_histories(histories, 20, key, nullptr, &touched);
}
}
UiCache::UiCache() : data_(json_pack("{s:{},s:{}}", "chats", "history")) {}
UiCache::~UiCache() { if (data_) json_decref(data_); if (startup_avatars_) json_decref(startup_avatars_); pthread_mutex_destroy(&mutex_); }
void UiCache::load(bool (*proceed)(void*), void* context) {
    // Synchronous on the existing Storage worker: its join owns the lifetime.
    lock_cache(&mutex_);
    if (generation_ != 0 || loaded_) { unlock_cache(&mutex_); return; }
    const auto generation = generation_;
    unlock_cache(&mutex_);
    bool cancelled = false;
    auto* loaded = read_cache(path, 8 * 1024 * 1024, proceed, context, &cancelled);
    if (cancelled) { if (loaded) json_decref(loaded); return; }
    if (!json_is_object(loaded) || !json_is_object(json_object_get(loaded, "chats"))) {
        if (loaded) json_decref(loaded);
        loaded = json_pack("{s:{},s:{}}", "chats", "history");
    }
    // Histories are kept in per-chat files now; a legacy cache still carries them here and is
    // migrated by the next flush.
    if (!json_is_object(json_object_get(loaded, "history"))) json_object_set_new(loaded, "history", json_object());
    std::vector<int64_t> legacy;
    {
        const char* chat_key; json_t* messages;
        json_object_foreach(json_object_get(loaded, "history"), chat_key, messages)
            legacy.push_back(std::strtoll(chat_key, nullptr, 10));
    }
    performance::Scope merging(performance::Metric::CacheMerge);
    auto* chats = json_object_get(loaded, "chats");
    const char* key; json_t* chat;
    json_object_foreach(chats, key, chat) {
        if (proceed && !proceed(context)) { json_decref(loaded); return; }
        json_object_set_new(chats, key, compact_chat(chat));
    }
    // Merge independent container copies outside the shared mutex. If live
    // data changed, retry from the unmodified disk tree, preserving deletions.
    while (!proceed || proceed(context)) {
        lock_cache(&mutex_);
        if (generation != generation_ || loaded_) { unlock_cache(&mutex_); break; }
        const auto revision = revision_;
        auto* current = frozenSnapshot();
        auto touched = early_history_;
        unlock_cache(&mutex_);
        auto* candidate = freeze_tree(loaded);
        if (!current || !candidate) {
            if (current) json_decref(current);
            if (candidate) json_decref(candidate);
            break;
        }
        for (auto id : touched) json_object_del(json_object_get(candidate, "history"), std::to_string(id).c_str());
        for (const char* collection : {"chats", "users", "history", "sticker_items"}) {
            auto* value = json_object_get(current, collection);
            if (!value) continue;
            auto* target = json_object_get(candidate, collection);
            if (json_is_object(target)) json_object_update(target, value);
            else json_object_set(candidate, collection, value);
        }
        for (const char* field : {"folders", "sticker_packs"})
            if (auto* value = json_object_get(current, field)) json_object_set(candidate, field, value);
        json_decref(current);
        // Trim to the runtime limits here, on Storage. A disk cache above them used
        // to be trimmed 16 entries per live update on the UI thread, ~4 ms per entry
        // (55-78 ms stalls per updateUser at startup).
        bool trimmed = true;
        for (const auto& limit : {std::pair<const char*, size_t>{"users", 1024}, {"chats", 2000}}) {
            auto* collection = json_object_get(candidate, limit.first);
            while (trimmed && json_is_object(collection) && json_object_size(collection) > limit.second) {
                if (proceed && !proceed(context)) trimmed = false;
                else retain(collection, limit.second, 16);
            }
        }
        if (!trimmed) { json_decref(candidate); break; }
        // Check scheduling/cancellation again before the constant-time publish.
        if (proceed && !proceed(context)) { json_decref(candidate); break; }
        lock_cache(&mutex_);
        const bool valid = generation == generation_ && !loaded_;
        const bool publish = valid && revision == revision_;
        json_t* retired = nullptr;
        if (publish) {
            retired = data_; data_ = candidate; candidate = nullptr;
            loaded_ = true; early_history_.clear();
            if (!legacy.empty()) {
                for (auto chat : legacy) history_dirty_.insert(chat);
                ++main_revision_; ++revision_;
            }
        }
        unlock_cache(&mutex_);
        if (retired) json_decref(retired);
        if (candidate) json_decref(candidate);
        if (publish) {
            // Only the list is read now; each file is merged later by loadHistory().
            auto files = history_files();
            lock_cache(&mutex_);
            if (generation == generation_ && !history_listed_) {
                for (auto chat : files) history_pending_.insert(chat);
                for (auto it = history_deleted_.begin(); it != history_deleted_.end();)
                    it = history_pending_.count(it->first) ? std::next(it) : history_deleted_.erase(it);
                history_listed_ = true;
            }
            unlock_cache(&mutex_);
        }
        if (publish || !valid) break;
    }
    json_decref(loaded);
}
bool UiCache::historyPending(int64_t id) {
    lock_cache(&mutex_);
    const bool pending = history_pending_.count(id) != 0;
    unlock_cache(&mutex_);
    return pending;
}
bool UiCache::loadHistory(int64_t preferred, bool (*proceed)(void*), void* context) {
    lock_cache(&mutex_);
    if (!loaded_ || history_pending_.empty()) { unlock_cache(&mutex_); return false; }
    const int64_t chat = history_pending_.count(preferred) ? preferred : *history_pending_.begin();
    const auto generation = generation_;
    unlock_cache(&mutex_);
    performance::Scope measurement(performance::Metric::CacheHistory);
    bool cancelled = false;
    json_t* disk = read_cache(history_file(chat).c_str(), history_file_limit, proceed, context, &cancelled);
    if (cancelled) { if (disk) json_decref(disk); return false; } // Stays pending for a later call.
    std::vector<json_t*> garbage;
    lock_cache(&mutex_);
    if (generation != generation_ || !history_pending_.erase(chat)) {
        unlock_cache(&mutex_);
        if (disk) json_decref(disk);
        return true;
    }
    std::unordered_set<int64_t> deleted;
    auto found = history_deleted_.find(chat);
    if (found != history_deleted_.end()) { deleted.swap(found->second); history_deleted_.erase(found); }
    json_t* histories = json_object_get(data_, "history");
    const std::string key = std::to_string(chat);
    json_t* items = json_object_get(histories, key.c_str());
    const bool live = json_object_size(items) != 0;
    if (json_is_object(disk) && json_object_size(disk)) {
        if (!items) { items = json_object(); json_object_set_new(histories, key.c_str(), items); }
        // Live messages win; disk ones only fill the gaps, unless deleted meanwhile.
        const char* id; json_t* message;
        json_object_foreach(disk, id, message) {
            const int64_t number_id = number(message, "id");
            if (!json_is_object(message) || number_id <= 0 || deleted.count(number_id) || json_object_get(items, id)) continue;
            json_object_set(items, id, message);
        }
        while (json_object_size(items) > 60) {
            const char* k; json_t* v; int64_t oldest = INT64_MAX; json_t* victim = nullptr;
            json_object_foreach(items, k, v) {
                const int64_t current = number(v, "id");
                if (current < oldest) { oldest = current; victim = v; }
            }
            if (victim) garbage.push_back(json_incref(victim));
            json_object_del(items, std::to_string(oldest).c_str());
        }
        ++revision_;
    }
    // A chat changed live before its merge is rewritten with the union.
    if (live || !deleted.empty()) { history_dirty_.insert(chat); ++revision_; }
    unlock_cache(&mutex_);
    if (disk) json_decref(disk);
    for (auto* value : garbage) json_decref(value);
    return true;
}
void UiCache::touchHistory(int64_t chat) {
    if (chat) history_dirty_.insert(chat);
}
void UiCache::removeHistoryFiles() {
    for (auto chat : history_files()) std::remove(history_file(chat).c_str());
}
json_t* UiCache::startupSnapshot() {
    performance::Scope measurement(performance::Metric::CacheStartupSnapshot);
    auto* frozen = json_object();
    lock_cache(&mutex_);
    // Leaves are immutable. Don't retain or copy the history containers at all.
    for (const char* key : {"chats", "users"})
        json_object_set_new(frozen, key, json_copy(json_object_get(data_, key)));
    json_object_set(frozen, "folders", json_object_get(data_, "folders"));
    unlock_cache(&mutex_);
    auto* result = startup_copy(frozen);
    json_decref(frozen);
    return result;
}
json_t* UiCache::loadStartup() {
    performance::Scope measurement(performance::Metric::CacheIndexRead);
    lock_cache(&mutex_);
    const auto generation = generation_;
    unlock_cache(&mutex_);
    if (generation) return nullptr;
    auto* result = read_cache(startup_path, 2 * 1024 * 1024);
    bool valid = number(result, "startup_format") == 2 &&
        json_is_object(json_object_get(result, "chats")) && json_is_object(json_object_get(result, "users"));
    const char* key; json_t* chat;
    json_object_foreach(json_object_get(result, "chats"), key, chat)
        if (!json_is_array(json_object_get(chat, "positions"))) valid = false;
    lock_cache(&mutex_);
    valid = valid && generation == generation_;
    unlock_cache(&mutex_);
    if (!valid) { if (result) json_decref(result); return nullptr; }
    auto* compact = startup_copy(result); json_decref(result);
    lock_cache(&mutex_);
    if (generation==generation_) {
        if (startup_avatars_) json_decref(startup_avatars_);
        startup_avatars_=json_incref(json_object_get(compact,"avatars"));
    }
    unlock_cache(&mutex_);
    return compact;
}
void UiCache::saveStartup(json_t* snapshot, bool (*proceed)(void*), void* context) {
    if (!snapshot) return;
    if (cache_writer::write(snapshot, startup_temp, 2 * 1024 * 1024, proceed, context) == cache_writer::Result::Ok)
        std::rename(startup_temp, startup_path);
}
uint64_t UiCache::generation() {
    lock_cache(&mutex_);
    const auto result = generation_;
    unlock_cache(&mutex_);
    return result;
}
bool UiCache::saveStartupList(json_t* chats, uint64_t generation, bool (*proceed)(void*), void* context) {
    lock_cache(&mutex_);
    // Before the full cache is merged, names and avatars of older chats are still on disk only.
    if (!loaded_ || !data_ || generation != generation_) { unlock_cache(&mutex_); return false; }
    // Cached leaves are immutable (replaced, never edited), so references are enough here.
    // Deep-copying every photo under the mutex stalled UI-thread remember() for 75-270 ms.
    // `chats` is owned by the caller and not shared: attach the photos in place.
    auto* cached = json_object_get(data_, "chats");
    const char* key; json_t* chat;
    json_object_foreach(chats, key, chat)
        if (auto* photo = json_object_get(json_object_get(cached, key), "photo")) json_object_set(chat, "photo", photo);
    auto* source = json_object();
    json_object_set(source, "chats", chats);
    json_object_set_new(source, "users", json_copy(json_object_get(data_, "users")));
    json_object_set(source, "folders", json_object_get(data_, "folders"));
    if (startup_avatars_) json_object_set(source, "avatars", startup_avatars_);
    unlock_cache(&mutex_);
    auto* startup = startup_copy(source);
    json_decref(source);
    lock_cache(&mutex_);
    const bool current = generation == generation_;
    unlock_cache(&mutex_);
    if (current) saveStartup(startup, proceed, context);
    json_decref(startup);
    return true;
}
json_t* UiCache::stickerSnapshot() {
    auto* frozen = json_object();
    lock_cache(&mutex_);
    json_object_set(frozen, "sticker_packs", json_object_get(data_, "sticker_packs"));
    unlock_cache(&mutex_);
    auto* result = measured_copy(frozen); json_decref(frozen); return result;
}
json_t* UiCache::chatAvatar(int64_t id) {
    lock_cache(&mutex_);
    auto* chat = json_object_get(json_object_get(data_, "chats"), std::to_string(id).c_str());
    auto* file=chat ? json_object_get(json_object_get(chat,"photo"),"small") : nullptr;
    // A live chat without a photo has none; one whose photo is not yet known as downloaded
    // falls back to the startup index's local picture.
    if (!chat || (file && !completed_file(file))) file=json_object_get(startup_avatars_,std::to_string(id).c_str());
    auto* result = measured_copy(file);
    unlock_cache(&mutex_);
    return result;
}
json_t* UiCache::frozenSnapshot() { return freeze_tree(data_); }

json_t* UiCache::snapshot() {
    lock_cache(&mutex_);
    json_t* frozen = frozenSnapshot();
    unlock_cache(&mutex_);
    json_t* result = measured_copy(frozen);
    if (frozen) json_decref(frozen);
    return result;
}
void UiCache::saveStickerPacks(json_t* value) {
    if (!value) return;
    auto* copy = measured_copy(value, "sticker_packs");  // outside the mutex
    lock_cache(&mutex_);
    if (data_) { json_object_set_new(data_, "sticker_packs", copy); ++revision_; ++main_revision_; }
    else json_decref(copy);
    unlock_cache(&mutex_);
}
void UiCache::saveStickerItems(int64_t set_id, json_t* value) {
    lock_cache(&mutex_);
    if (data_ && value) {
        json_t* items = json_object_get(data_, "sticker_items");
        if (!items) { items = json_object(); json_object_set_new(data_, "sticker_items", items); }
        set_copy(items, std::to_string(set_id).c_str(), value);
        while (json_object_size(items) > 8) {
            void* first = json_object_iter(items);
            if (!first) break;
            json_object_del(items, json_object_iter_key(first));
        }
        ++revision_; ++main_revision_;
    }
    unlock_cache(&mutex_);
}
json_t* UiCache::stickerItems(int64_t set_id) {
    lock_cache(&mutex_);
    json_t* all = data_ ? json_object_get(data_, "sticker_items") : nullptr;
    json_t* result = all ? measured_copy(json_object_get(all, std::to_string(set_id).c_str())) : nullptr;
    unlock_cache(&mutex_);
    return result;
}
void UiCache::clear() {
    lock_cache(&mutex_);
    if (data_) json_decref(data_);
    if (startup_avatars_) { json_decref(startup_avatars_); startup_avatars_=nullptr; }
    data_ = json_pack("{s:{},s:{}}", "chats", "history"); ++revision_; ++generation_; loaded_ = true; early_history_.clear();
    // Another account's histories must never be merged: the next flush removes every file.
    history_dirty_.clear(); history_pending_.clear(); history_deleted_.clear();
    history_listed_ = true; history_wipe_ = true; ++main_revision_;
    unlock_cache(&mutex_);
}
json_t* UiCache::chats() {
    lock_cache(&mutex_);
    json_t* result = measured_copy(json_object_get(data_, "chats"));
    unlock_cache(&mutex_); return result;
}
json_t* UiCache::users() {
    lock_cache(&mutex_);
    json_t* result = measured_copy(json_object_get(data_, "users"));
    unlock_cache(&mutex_); return result;
}
json_t* UiCache::folders() {
    lock_cache(&mutex_);
    json_t* result = measured_copy(json_object_get(data_, "folders"));
    unlock_cache(&mutex_); return result;
}
json_t* UiCache::history(int64_t id, size_t newest) {
    // Stored messages are immutable (every update replaces the object), so references are enough
    // under the mutex. Copying 60 messages here held it, and the UI opening a chat, for 150-190 ms.
    std::vector<std::pair<int64_t, json_t*>> messages;
    lock_cache(&mutex_);
    json_t* items = json_object_get(json_object_get(data_, "history"), std::to_string(id).c_str());
    if (!json_is_object(items)) { unlock_cache(&mutex_); return nullptr; }
    messages.reserve(json_object_size(items));
    const char* key; json_t* message;
    json_object_foreach(items, key, message) messages.push_back({number(message, "id"), json_incref(message)});
    unlock_cache(&mutex_);
    const size_t kept = std::min(newest, messages.size());
    std::partial_sort(messages.begin(), messages.begin() + kept, messages.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
    json_t* result = json_object();
    for (size_t i = 0; i < messages.size(); ++i) {
        if (i < kept) json_object_set_new(result, std::to_string(messages[i].first).c_str(), measured_copy(messages[i].second, "history"));
        json_decref(messages[i].second);
    }
    return result;
}
void UiCache::remember(json_t* root, uint64_t wire_bytes) {
    const char* t = type(root);
    // The Parser already stored this reply (see rememberMessages).
    if (!std::strcmp(t, "messages") && json_object_get(root, "@ui_remembered")) { json_object_del(root, "@ui_remembered"); return; }
    // These frequent events have no persisted UI fields. Avoid taking the cache
    // mutex and allocating chat/message lookup keys for each status snapshot.
    for (const char* transient : {"updateUserStatus", "updateConnectionState", "updateFile", "file", "updateOption", "ok", "error"})
        if (!std::strcmp(t, transient)) return;
    performance::Scope measurement(performance::Metric::UiRemember);
    struct MessagesTimer {
        bool on; int64_t started;
        ~MessagesTimer() { if (on) performance::observe(performance::Metric::MessagesRememberUs, performance::now() - started); }
    } messages_timer{!std::strcmp(t, "messages"), performance::now()};
    performance::SlowScope slow(performance::SlowPhase::Remember,t,wire_bytes);
    lock_cache(&mutex_);
    if (!data_) { unlock_cache(&mutex_); return; }
    if (!loaded_) {
        if (auto id = number(root, "chat_id")) early_history_.insert(id);
        if (auto id = number(json_object_get(root, "message"), "chat_id")) early_history_.insert(id);
    }
    json_t* chats = json_object_get(data_, "chats");
    json_t* history = json_object_get(data_, "history");
    slow.counts(json_object_size(chats),json_object_size(history));
    bool changed = false, main_changed = false;
    std::vector<int64_t> touched;
    if (!std::strcmp(t, "updatePoll")) {
        performance::SlowScope scan(performance::SlowPhase::PollScan,t);
        scan.counts(json_object_size(chats),json_object_size(history));
        json_t* poll = json_object_get(root, "poll");
        const char* chat_key; json_t* items;
        json_object_foreach(history, chat_key, items) {
            const char* message_key; json_t* message;
            json_object_foreach(items, message_key, message) {
                json_t* content = json_object_get(message, "content");
                json_t* old = json_object_get(content, "poll");
                if (old && json_equal(json_object_get(old, "id"), json_object_get(poll, "id"))) {
                    json_t* copy = measured_copy(message);
                    set_copy(json_object_get(copy, "content"), "poll", poll);
                    json_object_set_new(items, message_key, copy); changed = true;
                    touched.push_back(std::strtoll(chat_key, nullptr, 10));
                }
            }
        }
    }

    if (!std::strcmp(t, "updateChatFolders")) {
        json_object_set_new(data_, "folders", measured_copy(root)); changed = main_changed = true;
    }
    json_t* user = !std::strcmp(t, "user") ? root : !std::strcmp(t, "updateUser") ? json_object_get(root, "user") : nullptr;
    if (user) {
        json_t* users = json_object_get(data_, "users");
        if (!users) { users = json_object(); json_object_set_new(data_, "users", users); }
        json_t* name = json_object();
        for (const char* field : {"id", "first_name", "last_name", "profile_photo", "type"})
            set_copy(name, field, json_object_get(user, field));
        json_object_set_new(users, std::to_string(number(user, "id")).c_str(), name);
        retain(users, 1024, 16); changed = main_changed = true;
    }
    json_t* chat = !std::strcmp(t, "chat") ? root : !std::strcmp(t, "updateNewChat") ? json_object_get(root, "chat") : nullptr;
    if (chat) {
        json_t* copy = compact_chat(chat);
        json_object_set_new(chats, std::to_string(number(chat, "id")).c_str(), copy);
        retain(chats, 2000, 16); changed = main_changed = true; // The main snapshot is still capped at 8 MiB.
    }
    std::string chat_key = std::to_string(number(root, "chat_id"));
    json_t* cached = json_object_get(chats, chat_key.c_str());
    const bool chat_update = !std::strcmp(t, "updateChatTitle") || !std::strcmp(t, "updateChatPhoto") ||
        !std::strcmp(t, "updateChatLastMessage") || !std::strcmp(t, "updateChatDraftMessage") || !std::strcmp(t, "updateChatPosition");
    if (cached && chat_update) {
        cached = measured_copy(cached);
        json_object_set_new(chats, chat_key.c_str(), cached);
        main_changed = true;
    }
    if (cached && !std::strcmp(t, "updateChatTitle")) { set_copy(cached, "title", json_object_get(root, "title")); changed = true; }
    if (cached && !std::strcmp(t, "updateChatPhoto")) { set_copy(cached, "photo", json_object_get(root, "photo")); changed = true; }
    if (cached && (!std::strcmp(t, "updateChatLastMessage") || !std::strcmp(t, "updateChatDraftMessage"))) { set_copy(cached, "positions", json_object_get(root, "positions")); changed = true; }
    if (cached && !std::strcmp(t, "updateChatPosition")) {
        json_t* positions = json_object_get(cached, "positions");
        json_t* position = json_object_get(root, "position");
        if (!json_is_array(positions)) { positions = json_array(); json_object_set_new(cached, "positions", positions); }
        bool found = false;
        for (size_t i = 0; i < json_array_size(positions); ++i)
            if (json_equal(json_object_get(json_array_get(positions, i), "list"), json_object_get(position, "list"))) {
                json_array_set_new(positions, i, measured_copy(position)); found = true; break;
            }
        if (!found) json_array_append_new(positions, measured_copy(position));
        changed = true;
    }
    if (!std::strcmp(t, "messages")) {
        size_t i; json_t* m;
        json_array_foreach(json_object_get(root, "messages"), i, m) put_message(history, m, touched);
        changed = true;
    }
    if (!std::strcmp(t, "updateNewMessage") || !std::strcmp(t, "updateMessageSendSucceeded")) {
        put_message(history, json_object_get(root, "message"), touched); changed = true;
    }
    json_t* items = json_object_get(history, chat_key.c_str());
    const bool message_update = !std::strcmp(t, "updateMessageInteractionInfo") || !std::strcmp(t,"updateMessageReactions") ||
        !std::strcmp(t, "updateMessageContent") || !std::strcmp(t, "updateMessageEdited");
    if (items && message_update) {
        std::string id = std::to_string(number(root, "message_id"));
        json_t* m = json_object_get(items, id.c_str());
        if (m) json_object_set_new(items, id.c_str(), measured_copy(m));
    }
    if (items && !std::strcmp(t, "updateMessageInteractionInfo")) {
        json_t* m = json_object_get(items, std::to_string(number(root, "message_id")).c_str());
        if (m) { set_copy(m, "interaction_info", json_object_get(root, "interaction_info")); changed = true; }
    }
    if (items && !std::strcmp(t,"updateMessageReactions")) {
        auto* m=json_object_get(items,std::to_string(number(root,"message_id")).c_str());
        if (m) {
            auto* info=json_object_get(m,"interaction_info");
            if (!json_is_object(info)) { info=json_object();json_object_set_new(m,"interaction_info",info); }
            auto* reactions=json_object_get(info,"reactions");
            if (!json_is_object(reactions)) { reactions=json_object();json_object_set_new(info,"reactions",reactions); }
            set_copy(reactions,"reactions",json_object_get(root,"reactions"));changed=true;
        }
    }
    if (!std::strcmp(t, "message")) { put_message(history, root, touched); changed = true; }
    if (!std::strcmp(t, "updateDeleteMessages")) {
        // The chat's file may still be unmerged: remember the deletion for that merge.
        const int64_t chat_id = number(root, "chat_id");
        if (chat_id && (!history_listed_ || history_pending_.count(chat_id))) {
            auto& deleted = history_deleted_[chat_id];
            size_t i; json_t* id;
            json_array_foreach(json_object_get(root, "message_ids"), i, id)
                if (deleted.size() < 4096) deleted.insert(json_integer_value(id));
        }
    }
    if (items && !std::strcmp(t, "updateDeleteMessages")) {
        performance::SlowScope deletion(performance::SlowPhase::Delete,t);
        size_t i; json_t* id;
        json_array_foreach(json_object_get(root, "message_ids"), i, id)
            json_object_del(items, std::to_string(json_integer_value(id)).c_str());
        changed = true;
    }
    if (items && !std::strcmp(t, "updateMessageContent")) {
        std::string id = std::to_string(number(root, "message_id"));
        json_t* m = json_object_get(items, id.c_str());
        json_t* content = json_object_get(root, "new_content");
        if (json_is_true(json_object_get(content, "is_secret"))) json_object_del(items, id.c_str());
        else if (m) set_copy(m, "content", content);
        changed = true;
    }
    if (items && !std::strcmp(t, "updateMessageEdited")) {
        json_t* m = json_object_get(items, std::to_string(number(root, "message_id")).c_str());
        if (m) set_copy(m, "edit_date", json_object_get(root, "edit_date"));
        changed = true;
    }
    const bool message_change = message_update || !std::strcmp(t, "updateDeleteMessages");
    if (items && message_change) touched.push_back(number(root, "chat_id"));
    for (auto chat : touched) touchHistory(chat);
    if (main_changed) ++main_revision_;
    if (changed || main_changed) ++revision_;

    unlock_cache(&mutex_);
}
void UiCache::rememberMessages(json_t* root, uint64_t wire_bytes) {
    json_t* list = json_object_get(root, "messages");
    if (!json_is_array(list)) return;
    performance::Scope measurement(performance::Metric::UiRemember);
    const auto started = performance::now();
    performance::SlowScope slow(performance::SlowPhase::Remember, "messages", wire_bytes);
    struct Entry { int64_t chat, id; json_t* message; };
    std::vector<Entry> entries;
    size_t i; json_t* m;
    json_array_foreach(list, i, m) {
        const int64_t chat = number(m, "chat_id"), id = number(m, "id");
        // Same exclusions as put_message: unsent or self-destructing content.
        if (!chat || id <= 0 || json_is_object(json_object_get(m, "sending_state")) ||
            json_is_object(json_object_get(m, "self_destruct_type")) ||
            json_is_true(json_object_get(json_object_get(m, "content"), "is_secret"))) continue;
        entries.push_back({chat, id, m});
    }
    // A chat keeps its 60 newest messages; older ones of this reply would be evicted at once.
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.chat != b.chat ? a.chat < b.chat : a.id > b.id; });
    std::vector<Entry> kept; size_t per_chat = 0;
    for (const auto& e : entries) {
        per_chat = !kept.empty() && kept.back().chat == e.chat ? per_chat + 1 : 1;
        if (per_chat <= 60) kept.push_back(e);
    }
    // Deep copies are the expensive part (~1.7 ms/KB); do them without the mutex.
    for (auto& e : kept) e.message = measured_copy(e.message);
    lock_cache(&mutex_);
    if (!data_) {
        unlock_cache(&mutex_);
        for (auto& e : kept) json_decref(e.message);
        return;
    }
    json_t* history = json_object_get(data_, "history");
    slow.counts(json_object_size(json_object_get(data_, "chats")), json_object_size(history));
    // Replaced and evicted messages are freed after the mutex: a reloaded history replaces
    // up to 60 deep trees, and freeing them here held the UI thread's remember() for 60-370 ms.
    std::vector<json_t*> garbage;
    garbage.reserve(kept.size());
    std::vector<int64_t> touched;
    for (size_t n = 0; n < kept.size(); ++n) {
        const auto key = std::to_string(kept[n].chat);
        json_t* items = json_object_get(history, key.c_str());
        if (!items) { items = json_object(); json_object_set_new(history, key.c_str(), items); }
        const auto id = std::to_string(kept[n].id);
        if (auto* old = json_object_get(items, id.c_str())) garbage.push_back(json_incref(old));
        json_object_set_new(items, id.c_str(), kept[n].message);
        const bool last_of_chat = n + 1 == kept.size() || kept[n + 1].chat != kept[n].chat;
        if (!last_of_chat) continue;
        touched.push_back(kept[n].chat);
        // A reply stored before the legacy cache merge must win over that cache.
        if (!loaded_) early_history_.insert(kept[n].chat);
        while (json_object_size(items) > 60) {
            const char* k; json_t* v; int64_t oldest = INT64_MAX; json_t* victim = nullptr;
            json_object_foreach(items, k, v) {
                const int64_t current = number(v, "id");
                if (current < oldest) { oldest = current; victim = v; }
            }
            if (victim) garbage.push_back(json_incref(victim));
            json_object_del(items, std::to_string(oldest).c_str());
        }
        shrink_histories(history, 20, key, &garbage, &touched);
    }
    for (auto chat : touched) touchHistory(chat);
    if (!kept.empty()) ++revision_;
    unlock_cache(&mutex_);
    if (!garbage.empty()) {
        performance::Scope release(performance::Metric::JsonRelease);
        for (auto* value : garbage) json_decref(value);
    }
    performance::observe(performance::Metric::MessagesRememberUs, performance::now() - started);
}
void UiCache::flush(bool (*proceed)(void*), void* context) {
    performance::Scope measurement(performance::Metric::UiFlush);
    if (proceed && !proceed(context)) return;
    struct Pending { int64_t chat; json_t* items; }; // no items: the chat left the cache, its file goes
    std::vector<Pending> histories;
    lock_cache(&mutex_);
    if (!data_ || revision_ == saved_) { unlock_cache(&mutex_); return; }
    performance::Scope snapshot_time(performance::Metric::UiSnapshot);
    const uint64_t revision = revision_, main_revision = main_revision_, generation = generation_;
    // Each chat's history has its own file now: only the main file (chats, users, stickers)
    // and the changed chats are written, instead of rewriting up to 8 MiB on every change.
    json_t* copy = nullptr;
    if (main_revision != main_saved_) {
        copy = json_copy(data_);
        if (copy) {
            json_object_del(copy, "history");
            for (const char* key : {"chats", "users", "sticker_items"})
                if (auto* source = json_object_get(data_, key)) json_object_set_new(copy, key, json_copy(source));
        }
    }
    const bool wipe = history_wipe_;
    history_wipe_ = false;
    json_t* all = json_object_get(data_, "history");
    for (auto it = history_dirty_.begin(); it != history_dirty_.end();) {
        json_t* items = json_object_get(all, std::to_string(*it).c_str());
        const bool present = json_object_size(items) != 0;
        if (history_pending_.count(*it)) {
            // Live messages of a chat whose file is not merged yet would overwrite that file.
            if (present) { ++it; continue; }
            history_pending_.erase(*it); history_deleted_.erase(*it);
        }
        histories.push_back({*it, present ? json_copy(items) : nullptr}); // leaves are immutable
        it = history_dirty_.erase(it);
    }
    unlock_cache(&mutex_);
    snapshot_time.finish();
    if (main_revision != main_saved_ && !copy) {
        for (auto& h : histories) if (h.items) json_decref(h.items);
        lock_cache(&mutex_);
        if (generation == generation_) { for (auto& h : histories) history_dirty_.insert(h.chat); history_wipe_ |= wipe; }
        unlock_cache(&mutex_);
        return;
    }
    if (wipe) removeHistoryFiles();
    bool ok = true, interrupted = false;
    std::vector<int64_t> failed;
    auto current = [&] { lock_cache(&mutex_); const bool same = generation == generation_; unlock_cache(&mutex_); return same; };
    if (!histories.empty()) mkdir(history_dir, 0777);
    for (auto& h : histories) {
        const std::string file = history_file(h.chat), part = file + ".tmp";
        bool written = interrupted ? false : true;
        if (!h.items) {
            if (!interrupted) std::remove(file.c_str());
        } else if (!interrupted) {
            performance::Scope write_time(performance::Metric::UiWrite);
            auto result = cache_writer::write(h.items, part.c_str(), history_file_limit, proceed, context);
            // Captions of 60 media messages rarely reach the limit; the older half goes then.
            if (result == cache_writer::Result::Limit) {
                std::vector<int64_t> ids;
                const char* key; json_t* message;
                json_object_foreach(h.items, key, message) ids.push_back(number(message, "id"));
                std::sort(ids.begin(), ids.end());
                for (size_t n = 0; n < ids.size() / 2; ++n) json_object_del(h.items, std::to_string(ids[n]).c_str());
                result = cache_writer::write(h.items, part.c_str(), history_file_limit, proceed, context);
            }
            if (result == cache_writer::Result::Interrupted) interrupted = true;
            written = result == cache_writer::Result::Ok && current() && std::rename(part.c_str(), file.c_str()) == 0;
            if (!written) std::remove(part.c_str());
        }
        if (h.items) json_decref(h.items);
        if (!written) { ok = false; failed.push_back(h.chat); }
    }
    cache_writer::Result result = cache_writer::Result::Ok;
    if (copy && !interrupted) {
        // Keep startup reads bounded even if chat objects are unusually large.
        bool written = false;
        for (;;) {
            performance::Scope write_time(performance::Metric::UiWrite);
            result = cache_writer::write(copy, temp, 8*1024*1024, proceed, context);
            write_time.finish();
            if (result == cache_writer::Result::Ok) { written = true; break; }
            if (result != cache_writer::Result::Limit) break;
            json_t* collection = nullptr;
            for (const char* key : {"sticker_items", "chats", "users"}) {
                collection = json_object_get(copy, key);
                if (json_object_size(collection)) break;
            }
            size_t size = json_object_size(collection);
            if (!size) break;
            // Reduce a whole batch, not one item followed by another full disk write.
            retain(collection, size / 2);
        }
        // The startup index is written separately from the list the UI confirmed (saveStartupList).
        if (written) written = current() && std::rename(temp, path) == 0;
        if (!written) ok = false;
        else { lock_cache(&mutex_); main_saved_ = main_revision; unlock_cache(&mutex_); }
    } else if (copy) ok = false;
    if (copy) json_decref(copy);
    lock_cache(&mutex_);
    if (generation == generation_) {
        for (auto chat : failed) history_dirty_.insert(chat);
        if (ok) saved_ = revision;
    }
    unlock_cache(&mutex_);
    if (!ok && !interrupted && result != cache_writer::Result::Interrupted) std::fprintf(stderr, "UI cache write failed\n");
}
