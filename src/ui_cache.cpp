#include "performance.h"
#include "cache_writer.h"
#include "ui_cache.h"
#include "config.h"
#include <utility>
#include <sys/stat.h>
#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdio>
#ifndef JANSSON_THREAD_SAFE_REFCOUNT
#error Cache snapshots require Jansson atomic reference counting
#endif
namespace {
const char* path = VITAGRAM_DB_DIR "/ui-cache.json";
const char* temp = VITAGRAM_DB_DIR "/ui-cache.json.tmp";
const char* startup_path = VITAGRAM_DB_DIR "/ui-startup.json";
const char* startup_temp = VITAGRAM_DB_DIR "/ui-startup.json.tmp";
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
    if (!file.open(filename) || file.size() <= 0 || (uint64_t)file.size() > limit) return nullptr;
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
json_t* startup_copy(json_t* source) {
    auto* result = json_pack("{s:i,s:{},s:{}}", "startup_format", 2, "chats", "users");
    json_object_set_new(result,"avatars",json_object());
    const char* key; json_t* value;
    struct Row { std::string key; json_t* chat; int64_t order; };
    std::vector<Row> rows;
    json_object_foreach(json_object_get(source, "chats"), key, value) {
        int64_t order = 0;
        size_t i; json_t* position;
        json_array_foreach(json_object_get(value, "positions"), i, position)
            if (!std::strcmp(type(json_object_get(position, "list")), "chatListMain")) order = number(position, "order");
        rows.push_back({key, value, order});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.order != b.order ? a.order > b.order : number(a.chat, "id") > number(b.chat, "id");
    });
    for (const auto& row : rows) {
        auto* chat = json_object();
        auto* file=json_object_get(json_object_get(row.chat,"photo"),"small");
        if (!file) file=json_object_get(json_object_get(source,"avatars"),row.key.c_str());
        auto* local=json_object_get(file,"local");
        const char* local_path=json_string_value(json_object_get(local,"path"));
        if (json_is_true(json_object_get(local,"is_downloading_completed")) && local_path && std::strlen(local_path)<=1024) {
            auto* small=json_pack("{s:s,s:i}","@type","file","id",0);
            set_copy(small,"size",json_object_get(file,"size"));
            set_copy(small,"remote",json_object_get(file,"remote"));
            set_copy(small,"local",local);
            json_object_set_new(json_object_get(result,"avatars"),row.key.c_str(),small);
        }
        for (const char* field : {"@type", "id", "title", "type", "positions", "last_read_outbox_message_id"})
            set_copy(chat, field, json_object_get(row.chat, field));
        if (!json_is_array(json_object_get(chat, "positions"))) json_object_set_new(chat, "positions", json_array());
        json_object_set_new(json_object_get(result, "chats"), row.key.c_str(), chat);
    }
    json_object_foreach(json_object_get(source, "users"), key, value) {
        auto* user = json_object();
        for (const char* field : {"id", "first_name", "last_name"}) set_copy(user, field, json_object_get(value, field));
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
void shrink_histories(json_t* histories, size_t limit, const std::string& keep) {
    if (json_object_size(histories) <= limit) return;
    const char* victim_key = nullptr; json_t* victim = nullptr; const char* k; json_t* v;
    json_object_foreach(histories, k, v) if (keep != k) { victim_key = k; victim = v; break; }
    if (!victim) return;
    std::string victim_name = victim_key;
    for (int n = 0; n < 8; ++n) {
        void* first = json_object_iter(victim);
        if (!first) break;
        std::string message = json_object_iter_key(first);
        json_object_del(victim, message.c_str());
    }
    if (json_object_size(victim) == 0) json_object_del(histories, victim_name.c_str());
}
void put_message(json_t* histories, json_t* message) {
    int64_t chat = number(message, "chat_id"), id = number(message, "id");
    // Don't persist unsent messages or self-destructing content.
    if (!chat || id <= 0 || json_is_object(json_object_get(message, "sending_state")) ||
        json_is_object(json_object_get(message, "self_destruct_type")) ||
        json_is_true(json_object_get(json_object_get(message, "content"), "is_secret"))) return;
    std::string key = std::to_string(chat);
    json_t* items = json_object_get(histories, key.c_str());
    if (!items) { items = json_object(); json_object_set_new(histories, key.c_str(), items); }
    json_object_set_new(items, std::to_string(id).c_str(), measured_copy(message));
    while (json_object_size(items) > 60) {
        const char* k; json_t* v; int64_t oldest = INT64_MAX;
        json_object_foreach(items, k, v) oldest = std::min(oldest, number(v, "id"));
        json_object_del(items, std::to_string(oldest).c_str());
    }
    shrink_histories(histories, 20, key);
}
}
UiCache::UiCache() : data_(json_pack("{s:{},s:{}}", "chats", "history")) {}
UiCache::~UiCache() { if (data_) json_decref(data_); if (startup_avatars_) json_decref(startup_avatars_); pthread_mutex_destroy(&mutex_); }
void UiCache::load(bool (*proceed)(void*), void* context) {
    // Synchronous on the existing Storage worker: its join owns the lifetime.
    performance::lock(&mutex_, performance::Metric::UiLock);
    if (generation_ != 0 || loaded_) { pthread_mutex_unlock(&mutex_); return; }
    const auto generation = generation_;
    pthread_mutex_unlock(&mutex_);
    bool cancelled = false;
    auto* loaded = read_cache(path, 8 * 1024 * 1024, proceed, context, &cancelled);
    if (cancelled) { if (loaded) json_decref(loaded); return; }
    if (!json_is_object(loaded) || !json_is_object(json_object_get(loaded, "chats")) ||
        !json_is_object(json_object_get(loaded, "history"))) {
        if (loaded) json_decref(loaded);
        loaded = json_pack("{s:{},s:{}}", "chats", "history");
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
        performance::lock(&mutex_, performance::Metric::UiLock);
        if (generation != generation_ || loaded_) { pthread_mutex_unlock(&mutex_); break; }
        const auto revision = revision_;
        auto* current = frozenSnapshot();
        auto touched = early_history_;
        pthread_mutex_unlock(&mutex_);
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
        performance::lock(&mutex_, performance::Metric::UiLock);
        const bool valid = generation == generation_ && !loaded_;
        const bool publish = valid && revision == revision_;
        json_t* retired = nullptr;
        if (publish) {
            retired = data_; data_ = candidate; candidate = nullptr;
            loaded_ = true; early_history_.clear();
        }
        pthread_mutex_unlock(&mutex_);
        if (retired) json_decref(retired);
        if (candidate) json_decref(candidate);
        if (publish || !valid) break;
    }
    json_decref(loaded);
}
json_t* UiCache::startupSnapshot() {
    performance::Scope measurement(performance::Metric::CacheStartupSnapshot);
    auto* frozen = json_object();
    performance::lock(&mutex_, performance::Metric::UiLock);
    // Leaves are immutable. Don't retain or copy the history containers at all.
    for (const char* key : {"chats", "users"})
        json_object_set_new(frozen, key, json_copy(json_object_get(data_, key)));
    json_object_set(frozen, "folders", json_object_get(data_, "folders"));
    pthread_mutex_unlock(&mutex_);
    auto* result = startup_copy(frozen);
    json_decref(frozen);
    return result;
}
json_t* UiCache::loadStartup() {
    performance::Scope measurement(performance::Metric::CacheIndexRead);
    performance::lock(&mutex_, performance::Metric::UiLock);
    const auto generation = generation_;
    pthread_mutex_unlock(&mutex_);
    if (generation) return nullptr;
    auto* result = read_cache(startup_path, 2 * 1024 * 1024);
    bool valid = number(result, "startup_format") == 2 &&
        json_is_object(json_object_get(result, "chats")) && json_is_object(json_object_get(result, "users"));
    const char* key; json_t* chat;
    json_object_foreach(json_object_get(result, "chats"), key, chat)
        if (!json_is_array(json_object_get(chat, "positions"))) valid = false;
    performance::lock(&mutex_, performance::Metric::UiLock);
    valid = valid && generation == generation_;
    pthread_mutex_unlock(&mutex_);
    if (!valid) { if (result) json_decref(result); return nullptr; }
    auto* compact = startup_copy(result); json_decref(result);
    performance::lock(&mutex_,performance::Metric::UiLock);
    if (generation==generation_) {
        if (startup_avatars_) json_decref(startup_avatars_);
        startup_avatars_=json_incref(json_object_get(compact,"avatars"));
    }
    pthread_mutex_unlock(&mutex_);
    return compact;
}
void UiCache::saveStartup(json_t* snapshot, bool (*proceed)(void*), void* context) {
    if (!snapshot) return;
    if (cache_writer::write(snapshot, startup_temp, 2 * 1024 * 1024, proceed, context) == cache_writer::Result::Ok)
        std::rename(startup_temp, startup_path);
}
json_t* UiCache::stickerSnapshot() {
    auto* frozen = json_object();
    performance::lock(&mutex_, performance::Metric::UiLock);
    json_object_set(frozen, "sticker_packs", json_object_get(data_, "sticker_packs"));
    pthread_mutex_unlock(&mutex_);
    auto* result = measured_copy(frozen); json_decref(frozen); return result;
}
json_t* UiCache::chatAvatar(int64_t id) {
    performance::lock(&mutex_, performance::Metric::UiLock);
    auto* chat = json_object_get(json_object_get(data_, "chats"), std::to_string(id).c_str());
    auto* file=chat ? json_object_get(json_object_get(chat,"photo"),"small") : json_object_get(startup_avatars_,std::to_string(id).c_str());
    auto* result = measured_copy(file);
    pthread_mutex_unlock(&mutex_);
    return result;
}
json_t* UiCache::frozenSnapshot() { return freeze_tree(data_); }

json_t* UiCache::snapshot() {
    performance::lock(&mutex_, performance::Metric::UiLock);
    json_t* frozen = frozenSnapshot();
    pthread_mutex_unlock(&mutex_);
    json_t* result = measured_copy(frozen);
    if (frozen) json_decref(frozen);
    return result;
}
void UiCache::saveStickerPacks(json_t* value) {
    if (!value) return;
    auto* copy = measured_copy(value, "sticker_packs");  // outside the mutex
    performance::lock(&mutex_, performance::Metric::UiLock);
    if (data_) { json_object_set_new(data_, "sticker_packs", copy); ++revision_; }
    else json_decref(copy);
    pthread_mutex_unlock(&mutex_);
}
void UiCache::saveStickerItems(int64_t set_id, json_t* value) {
    performance::lock(&mutex_, performance::Metric::UiLock);
    if (data_ && value) {
        json_t* items = json_object_get(data_, "sticker_items");
        if (!items) { items = json_object(); json_object_set_new(data_, "sticker_items", items); }
        set_copy(items, std::to_string(set_id).c_str(), value);
        while (json_object_size(items) > 8) {
            void* first = json_object_iter(items);
            if (!first) break;
            json_object_del(items, json_object_iter_key(first));
        }
        ++revision_;
    }
    pthread_mutex_unlock(&mutex_);
}
json_t* UiCache::stickerItems(int64_t set_id) {
    performance::lock(&mutex_, performance::Metric::UiLock);
    json_t* all = data_ ? json_object_get(data_, "sticker_items") : nullptr;
    json_t* result = all ? measured_copy(json_object_get(all, std::to_string(set_id).c_str())) : nullptr;
    pthread_mutex_unlock(&mutex_);
    return result;
}
void UiCache::clear() {
    performance::lock(&mutex_, performance::Metric::UiLock);
    if (data_) json_decref(data_);
    if (startup_avatars_) { json_decref(startup_avatars_); startup_avatars_=nullptr; }
    data_ = json_pack("{s:{},s:{}}", "chats", "history"); ++revision_; ++generation_; loaded_ = true; early_history_.clear();
    pthread_mutex_unlock(&mutex_);
}
json_t* UiCache::chats() {
    performance::lock(&mutex_, performance::Metric::UiLock);
    json_t* result = measured_copy(json_object_get(data_, "chats"));
    pthread_mutex_unlock(&mutex_); return result;
}
json_t* UiCache::users() {
    performance::lock(&mutex_, performance::Metric::UiLock);
    json_t* result = measured_copy(json_object_get(data_, "users"));
    pthread_mutex_unlock(&mutex_); return result;
}
json_t* UiCache::folders() {
    performance::lock(&mutex_, performance::Metric::UiLock);
    json_t* result = measured_copy(json_object_get(data_, "folders"));
    pthread_mutex_unlock(&mutex_); return result;
}
json_t* UiCache::history(int64_t id) {
    performance::lock(&mutex_, performance::Metric::UiLock);
    json_t* result = measured_copy(json_object_get(json_object_get(data_, "history"), std::to_string(id).c_str()));
    pthread_mutex_unlock(&mutex_); return result;
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
    performance::lock(&mutex_, performance::Metric::UiLock);
    if (!data_) { pthread_mutex_unlock(&mutex_); return; }
    if (!loaded_) {
        if (auto id = number(root, "chat_id")) early_history_.insert(id);
        if (auto id = number(json_object_get(root, "message"), "chat_id")) early_history_.insert(id);
    }
    json_t* chats = json_object_get(data_, "chats");
    json_t* history = json_object_get(data_, "history");
    slow.counts(json_object_size(chats),json_object_size(history));
    bool changed = false;
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
                }
            }
        }
    }

    if (!std::strcmp(t, "updateChatFolders")) {
        json_object_set_new(data_, "folders", measured_copy(root)); changed = true;
    }
    json_t* user = !std::strcmp(t, "user") ? root : !std::strcmp(t, "updateUser") ? json_object_get(root, "user") : nullptr;
    if (user) {
        json_t* users = json_object_get(data_, "users");
        if (!users) { users = json_object(); json_object_set_new(data_, "users", users); }
        json_t* name = json_object();
        for (const char* field : {"id", "first_name", "last_name", "profile_photo"})
            set_copy(name, field, json_object_get(user, field));
        json_object_set_new(users, std::to_string(number(user, "id")).c_str(), name);
        retain(users, 1024, 16); changed = true;
    }
    json_t* chat = !std::strcmp(t, "chat") ? root : !std::strcmp(t, "updateNewChat") ? json_object_get(root, "chat") : nullptr;
    if (chat) {
        json_t* copy = compact_chat(chat);
        json_object_set_new(chats, std::to_string(number(chat, "id")).c_str(), copy);
        retain(chats, 2000, 16); changed = true; // Overall disk snapshot is still capped at 8 MiB.
    }
    std::string chat_key = std::to_string(number(root, "chat_id"));
    json_t* cached = json_object_get(chats, chat_key.c_str());
    const bool chat_update = !std::strcmp(t, "updateChatTitle") || !std::strcmp(t, "updateChatPhoto") ||
        !std::strcmp(t, "updateChatLastMessage") || !std::strcmp(t, "updateChatDraftMessage") || !std::strcmp(t, "updateChatPosition");
    if (cached && chat_update) {
        cached = measured_copy(cached);
        json_object_set_new(chats, chat_key.c_str(), cached);
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
        json_array_foreach(json_object_get(root, "messages"), i, m) put_message(history, m);
        changed = true;
    }
    if (!std::strcmp(t, "updateNewMessage") || !std::strcmp(t, "updateMessageSendSucceeded")) {
        put_message(history, json_object_get(root, "message")); changed = true;
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
    if (!std::strcmp(t, "message")) { put_message(history, root); changed = true; }
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
    if (changed) ++revision_;

    pthread_mutex_unlock(&mutex_);
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
    performance::lock(&mutex_, performance::Metric::UiLock);
    if (!data_) {
        pthread_mutex_unlock(&mutex_);
        for (auto& e : kept) json_decref(e.message);
        return;
    }
    json_t* history = json_object_get(data_, "history");
    slow.counts(json_object_size(json_object_get(data_, "chats")), json_object_size(history));
    for (size_t n = 0; n < kept.size(); ++n) {
        const auto key = std::to_string(kept[n].chat);
        json_t* items = json_object_get(history, key.c_str());
        if (!items) { items = json_object(); json_object_set_new(history, key.c_str(), items); }
        json_object_set_new(items, std::to_string(kept[n].id).c_str(), kept[n].message);
        const bool last_of_chat = n + 1 == kept.size() || kept[n + 1].chat != kept[n].chat;
        if (!last_of_chat) continue;
        while (json_object_size(items) > 60) {
            const char* k; json_t* v; int64_t oldest = INT64_MAX;
            json_object_foreach(items, k, v) oldest = std::min(oldest, number(v, "id"));
            json_object_del(items, std::to_string(oldest).c_str());
        }
        shrink_histories(history, 20, key);
    }
    if (!kept.empty()) ++revision_;
    pthread_mutex_unlock(&mutex_);
    performance::observe(performance::Metric::MessagesRememberUs, performance::now() - started);
}
void UiCache::flush(bool (*proceed)(void*), void* context) {
    performance::Scope measurement(performance::Metric::UiFlush);
    if (proceed && !proceed(context)) return;
    performance::lock(&mutex_, performance::Metric::UiLock);
    if (!data_ || revision_ == saved_) { pthread_mutex_unlock(&mutex_); return; }
    performance::Scope snapshot_time(performance::Metric::UiSnapshot);
    uint64_t revision = revision_; json_t* copy = frozenSnapshot();
    pthread_mutex_unlock(&mutex_);
    snapshot_time.finish();
    if (!copy) return;
    // Keep startup reads bounded even if chat objects or captions are unusually large.
    bool ok = false;
    cache_writer::Result result = cache_writer::Result::Error;
    for (;;) {
        performance::Scope write_time(performance::Metric::UiWrite);
        result = cache_writer::write(copy, temp, 8*1024*1024, proceed, context);
        write_time.finish();
        if (result == cache_writer::Result::Ok) { ok = true; break; }
        if (result != cache_writer::Result::Limit) break;
        json_t* collection = nullptr;
        for (const char* key : {"history", "sticker_items", "chats", "users"}) {
            collection = json_object_get(copy, key);
            if (json_object_size(collection)) break;
        }
        size_t size = json_object_size(collection);
        if (!size) break;
        // Reduce a whole batch, not one item followed by another full disk write.
        retain(collection, size / 2);
    }
    if (ok) ok = std::rename(temp, path) == 0;
    if (ok) {
        auto* startup = startup_copy(copy);
        saveStartup(startup, proceed, context);
        json_decref(startup);
    }
    json_decref(copy);
    if (ok) { performance::lock(&mutex_, performance::Metric::UiLock); saved_ = revision; pthread_mutex_unlock(&mutex_); }
    else if (result != cache_writer::Result::Interrupted) std::fprintf(stderr, "UI cache write failed\n");
}
