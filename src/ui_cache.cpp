#include "ui_cache.h"
#include "config.h"
#include <sys/stat.h>
#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
namespace {
const char* path = VITAGRAM_DB_DIR "/ui-cache.json";
const char* temp = VITAGRAM_DB_DIR "/ui-cache.json.tmp";
const char* type(json_t* o) { const char* s = json_string_value(json_object_get(o, "@type")); return s ? s : ""; }
int64_t number(json_t* o, const char* key) { return json_integer_value(json_object_get(o, key)); }
void retain(json_t* object, size_t limit) {
    while (json_object_size(object) > limit) {
        void* first = json_object_iter(object);
        if (!first) break;
        std::string key = json_object_iter_key(first); json_object_del(object, key.c_str());
    }
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
    json_object_set_new(items, std::to_string(id).c_str(), json_deep_copy(message));
    while (json_object_size(items) > 60) {
        const char* k; json_t* v; int64_t oldest = INT64_MAX;
        json_object_foreach(items, k, v) oldest = std::min(oldest, number(v, "id"));
        json_object_del(items, std::to_string(oldest).c_str());
    }
    retain(histories, 20);
}
}
UiCache::~UiCache() { if (data_) json_decref(data_); pthread_mutex_destroy(&mutex_); }
void UiCache::load() {
    struct stat info{}; json_error_t error{};
    if (!stat(path, &info) && info.st_size > 0 && info.st_size <= 8 * 1024 * 1024) data_ = json_load_file(path, 0, &error);
    if (!json_is_object(data_) || !json_is_object(json_object_get(data_, "chats")) || !json_is_object(json_object_get(data_, "history"))) {
        if (data_) json_decref(data_);
        data_ = json_pack("{s:{},s:{}}", "chats", "history");
    }
}
void UiCache::clear() {
    pthread_mutex_lock(&mutex_);
    if (data_) json_decref(data_);
    data_ = json_pack("{s:{},s:{}}", "chats", "history"); ++revision_;
    pthread_mutex_unlock(&mutex_);
}
json_t* UiCache::chats() {
    pthread_mutex_lock(&mutex_);
    json_t* result = json_deep_copy(json_object_get(data_, "chats"));
    pthread_mutex_unlock(&mutex_); return result;
}
json_t* UiCache::users() {
    pthread_mutex_lock(&mutex_);
    json_t* result = json_deep_copy(json_object_get(data_, "users"));
    pthread_mutex_unlock(&mutex_); return result;
}
json_t* UiCache::folders() {
    pthread_mutex_lock(&mutex_);
    json_t* result = json_deep_copy(json_object_get(data_, "folders"));
    pthread_mutex_unlock(&mutex_); return result;
}
json_t* UiCache::history(int64_t id) {
    pthread_mutex_lock(&mutex_);
    json_t* result = json_deep_copy(json_object_get(json_object_get(data_, "history"), std::to_string(id).c_str()));
    pthread_mutex_unlock(&mutex_); return result;
}
void UiCache::remember(json_t* root) {
    pthread_mutex_lock(&mutex_);
    if (!data_) { pthread_mutex_unlock(&mutex_); return; }
    const char* t = type(root);
    json_t* chats = json_object_get(data_, "chats");
    json_t* history = json_object_get(data_, "history");
    bool changed = false;
    if (!std::strcmp(t, "updateChatFolders")) {
        json_object_set_new(data_, "folders", json_deep_copy(root)); changed = true;
    }
    json_t* user = !std::strcmp(t, "user") ? root : !std::strcmp(t, "updateUser") ? json_object_get(root, "user") : nullptr;
    if (user) {
        json_t* users = json_object_get(data_, "users");
        if (!users) { users = json_object(); json_object_set_new(data_, "users", users); }
        json_t* name = json_object();
        for (const char* field : {"id", "first_name", "last_name"})
            json_object_set(name, field, json_object_get(user, field));
        json_object_set_new(users, std::to_string(number(user, "id")).c_str(), name);
        retain(users, 1024); changed = true;
    }
    json_t* chat = !std::strcmp(t, "chat") ? root : !std::strcmp(t, "updateNewChat") ? json_object_get(root, "chat") : nullptr;
    if (chat) {
        json_t* copy = json_deep_copy(chat);
        // Last-message content is unnecessary for the list; don't persist it here.
        json_object_del(copy, "last_message");
        json_object_set_new(chats, std::to_string(number(chat, "id")).c_str(), copy);
        retain(chats, 2000); changed = true; // Overall disk snapshot is still capped at 8 MiB.
    }
    std::string chat_key = std::to_string(number(root, "chat_id"));
    json_t* cached = json_object_get(chats, chat_key.c_str());
    if (cached && !std::strcmp(t, "updateChatTitle")) { json_object_set(cached, "title", json_object_get(root, "title")); changed = true; }
    if (cached && !std::strcmp(t, "updateChatPhoto")) { json_object_set(cached, "photo", json_object_get(root, "photo")); changed = true; }
    if (cached && !std::strcmp(t, "updateChatLastMessage")) { json_object_set(cached, "positions", json_object_get(root, "positions")); changed = true; }
    if (cached && !std::strcmp(t, "updateChatPosition")) {
        json_t* positions = json_object_get(cached, "positions");
        json_t* position = json_object_get(root, "position");
        if (!json_is_array(positions)) { positions = json_array(); json_object_set_new(cached, "positions", positions); }
        bool found = false;
        for (size_t i = 0; i < json_array_size(positions); ++i)
            if (json_equal(json_object_get(json_array_get(positions, i), "list"), json_object_get(position, "list"))) {
                json_array_set(positions, i, position); found = true; break;
            }
        if (!found) json_array_append(positions, position);
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
    if (items && !std::strcmp(t, "updateMessageInteractionInfo")) {
        json_t* m = json_object_get(items, std::to_string(number(root, "message_id")).c_str());
        if (m) { json_object_set(m, "interaction_info", json_object_get(root, "interaction_info")); changed = true; }
    }
    if (!std::strcmp(t, "message")) { put_message(history, root); changed = true; }
    if (items && !std::strcmp(t, "updateDeleteMessages")) {
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
        else if (m) json_object_set(m, "content", content);
        changed = true;
    }
    if (items && !std::strcmp(t, "updateMessageEdited")) {
        json_t* m = json_object_get(items, std::to_string(number(root, "message_id")).c_str());
        if (m) json_object_set(m, "edit_date", json_object_get(root, "edit_date"));
        changed = true;
    }
    if (changed) ++revision_;
    pthread_mutex_unlock(&mutex_);
}
void UiCache::flush() {
    pthread_mutex_lock(&mutex_);
    if (!data_ || revision_ == saved_) { pthread_mutex_unlock(&mutex_); return; }
    uint64_t revision = revision_; json_t* copy = json_deep_copy(data_);
    pthread_mutex_unlock(&mutex_);
    if (!copy) return;
    // Keep startup reads bounded even if chat objects or captions are unusually large.
    bool ok = false;
    for (;;) {
        if (json_dump_file(copy, temp, JSON_COMPACT)) break;
        struct stat info{};
        if (stat(temp, &info)) break;
        if (info.st_size <= 8 * 1024 * 1024) { ok = true; break; }
        json_t* collection = json_object_get(copy, "history");
        if (!json_object_size(collection)) collection = json_object_get(copy, "chats");
        size_t size = json_object_size(collection);
        if (!size) break;
        retain(collection, size - 1);
    }
    json_decref(copy);
    if (ok) ok = std::rename(temp, path) == 0;
    if (ok) { pthread_mutex_lock(&mutex_); saved_ = revision; pthread_mutex_unlock(&mutex_); }
    else std::fprintf(stderr, "UI cache write failed\n");
}
