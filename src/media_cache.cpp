#include "media_cache.h"
#include "config.h"
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
const char* index_path = VITAGRAM_DB_DIR "/media-cache.json";
const char* temp_path = VITAGRAM_DB_DIR "/media-cache.json.tmp";
const char* str(json_t* obj, const char* key) {
    const char* value = json_string_value(json_object_get(obj, key));
    return value ? value : "";
}
bool valid_path(const char* path, int64_t size) {
    std::string prefix = std::string(VITAGRAM_FILES_DIR) + "/";
    std::string normalized = path;
    auto colon = normalized.find(':');
    if (colon != std::string::npos && normalized.compare(colon + 1, 1, "/") == 0)
        normalized.erase(colon + 1, 1); // TDLib may return ux0:/data instead of ux0:data.
    bool allowed = normalized.compare(0, prefix.size(), prefix) == 0;
    // TDLib keeps these media classes under database_directory by default
    // (FileDirType::Secure), not under files_directory. Never allow the DB root.
    for (const char* directory : {"profile_photos/", "thumbnails/", "stickers/"}) {
        std::string media_prefix = std::string(VITAGRAM_DB_DIR) + "/" + directory;
        if (normalized.compare(0, media_prefix.size(), media_prefix) == 0) allowed = true;
    }
    if (!allowed || std::strstr(path, "..") || std::strchr(path, '\\')) return false;
    struct stat info{};
    return size > 0 && !stat(path, &info) && S_ISREG(info.st_mode) && info.st_size == size;
}
}
MediaCache::~MediaCache() { if (index_) json_decref(index_); pthread_mutex_destroy(&mutex_); }
void MediaCache::load() {
    if (index_) { json_decref(index_); index_ = nullptr; }
    struct stat info{};
    json_error_t error{};
    if (!stat(index_path, &info) && info.st_size > 0 && info.st_size <= 4 * 1024 * 1024)
        index_ = json_load_file(index_path, 0, &error);
    if (!json_is_object(index_) || json_object_size(index_) > 8192) { if (index_) json_decref(index_); index_ = json_object(); }
}
void MediaCache::resolve(json_t* root) {
    pthread_mutex_lock(&mutex_);
    if (index_) walk(root, 0);
    pthread_mutex_unlock(&mutex_);
}
void MediaCache::walk(json_t* root, unsigned depth) {
    if (depth > 32) return;
    if (json_is_array(root)) {
        size_t i; json_t* item;
        json_array_foreach(root, i, item) walk(item, depth + 1);
    } else if (json_is_object(root)) {
        if (!std::strcmp(str(root, "@type"), "file")) {
            const char* key = str(json_object_get(root, "remote"), "unique_id");
            json_t* local = json_object_get(root, "local");
            if (!key[0] || std::strlen(key) > 256 || !json_is_object(local)) return;
            json_t* old = json_object_get(index_, key);
            if (json_is_true(json_object_get(local, "is_downloading_completed"))) {
                const char* path = str(local, "path");
                int64_t size = json_integer_value(json_object_get(root, "size"));
                if (size <= 0) size = json_integer_value(json_object_get(local, "downloaded_size"));
                bool valid = valid_path(path, size);
                if (valid && (std::strcmp(str(old, "path"), path) || json_integer_value(json_object_get(old, "size")) != size)) {
                    // Bound metadata, never delete the underlying media files.
                    if (!old && json_object_size(index_) >= 8192) return;
                    json_object_set_new(index_, key, json_pack("{s:s,s:I}", "path", path, "size", (json_int_t)size));
                    ++revision_;
                }
                if (valid) return;
                json_object_set_new(local, "path", json_string(""));
                json_object_set_new(local, "is_downloading_completed", json_false());
            }
            const char* path = str(old, "path");
            int64_t size = json_integer_value(json_object_get(old, "size"));
            int64_t expected = json_integer_value(json_object_get(root, "size"));
            if (old && (!expected || expected == size) && valid_path(path, size)) {
                ++hits_;
                json_object_set_new(local, "path", json_string(path));
                json_object_set_new(local, "is_downloading_completed", json_true());
                json_object_set_new(local, "is_downloading_active", json_false());
                json_object_set_new(local, "downloaded_size", json_integer(size));
                json_object_set_new(local, "downloaded_prefix_size", json_integer(size));
            } else if (old) { json_object_del(index_, key); ++revision_; }
            return;
        }
        const char* key; json_t* value;
        json_object_foreach(root, key, value) walk(value, depth + 1);
    }
}
void MediaCache::flush() {
    pthread_mutex_lock(&mutex_);
    if (!index_ || revision_ == saved_) { pthread_mutex_unlock(&mutex_); return; }
    uint64_t revision = revision_;
    json_t* copy = json_deep_copy(index_);
    pthread_mutex_unlock(&mutex_);
    if (!copy) return;
    bool ok = json_dump_file(copy, temp_path, JSON_COMPACT) == 0;
    json_decref(copy);
    if (ok) ok = std::rename(temp_path, index_path) == 0;
    if (ok) {
        pthread_mutex_lock(&mutex_); saved_ = revision; pthread_mutex_unlock(&mutex_);
    } else std::fprintf(stderr, "Media cache: index write failed; existing index retained\n");
}
std::string MediaCache::status() {
    pthread_mutex_lock(&mutex_);
    std::string result = "Media index: " + std::to_string(index_ ? json_object_size(index_) : 0) +
        " | reused: " + std::to_string(hits_) + (revision_ != saved_ ? " | saving" : " | saved");
    pthread_mutex_unlock(&mutex_);
    return result;
}
