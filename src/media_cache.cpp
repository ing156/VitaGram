#include "performance.h"
#include "cache_writer.h"
#include "media_cache.h"
#include "config.h"
#include "vault.h"
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <chrono>

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
    // A sealed copy is larger than the plain size TDLib reports by a fixed, computable amount.
    return size > 0 && !stat(path, &info) && S_ISREG(info.st_mode) &&
           (info.st_size == size || info.st_size == vault::sealed_size(size));
}
}
void MediaCache::lockAs(const char* who, int line) {
    if (!performance::enabled) { pthread_mutex_lock(&mutex_); return; }
    const char* blocker = nullptr; int blocker_line = 0;
    const auto started = performance::now();
    if (pthread_mutex_trylock(&mutex_)) {
        blocker = holder_.load(); blocker_line = holder_line_.load();
        pthread_mutex_lock(&mutex_);
    }
    const auto waited = performance::now() - started;
    performance::observe(performance::Metric::MediaLock, waited);
    if (waited >= 50000) {
        char name[48];
        std::snprintf(name, sizeof(name), "mlw_%s_%d_by_%s_%d", who, line, blocker ? blocker : "unknown", blocker_line);
        performance::SlowScope slow(performance::SlowPhase::Ui, name);
        slow.since(started);
    }
    holder_.store(who); holder_line_.store(line); held_since_ = performance::now();
}
void MediaCache::unlockHeld() {
    if (performance::enabled && held_since_) {
        const auto held = performance::now() - held_since_;
        if (held >= 50000) {
            char name[48];
            std::snprintf(name, sizeof(name), "mlh_%s_%d", holder_.load() ? holder_.load() : "unknown", holder_line_.load());
            performance::SlowScope slow(performance::SlowPhase::Ui, name);
            slow.since(held_since_);
        }
    }
    held_since_ = 0; holder_.store(nullptr);
    pthread_mutex_unlock(&mutex_);
}
MediaCache::~MediaCache() { clearPending(); if (index_) json_decref(index_); pthread_mutex_destroy(&mutex_); }
void MediaCache::clearPending() {
    for (auto& entry : pending_) json_decref(entry.second.file);
    pending_.clear();
}
void MediaCache::clear() {
    lockAs(__func__, __LINE__);
    if (index_) json_decref(index_);
    index_ = json_object(); ++revision_; ++generation_; validated_.clear();
    clearPending();
    unlockHeld();
}
void MediaCache::load() {
    lockAs(__func__, __LINE__);
    if (generation_ != 0) { unlockHeld(); return; }
    const auto generation = generation_;
    unlockHeld();
    json_error_t error{};
    json_t* loaded = nullptr;
    std::string text; // the index is sealed with the rest of the cache when the vault is on
    if (vault::read_file(index_path, 4 * 1024 * 1024, text, nullptr, nullptr,
                         vault::caches_must_be_sealed() ? vault::Require::Vault : vault::Require::Any) && !text.empty())
        loaded = json_loadb(text.data(), text.size(), 0, &error);
    if (!json_is_object(loaded) || json_object_size(loaded) > 8192) { if (loaded) json_decref(loaded); loaded = json_object(); }
    lockAs(__func__, __LINE__);
    if (generation == generation_) {
        if (index_) json_object_update(loaded, index_); // Live records win.
        if (index_) json_decref(index_);
        index_ = loaded; loaded = nullptr;
    }
    unlockHeld();
    if (loaded) json_decref(loaded);
}
namespace {
std::string validation_key(const char* path, int64_t size) {
    return std::string(path) + "\n" + std::to_string(size);
}
int64_t validation_clock() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
bool MediaCache::checked(const char* path, int64_t size) const {
    auto it = validated_.find(validation_key(path, size));
    return it != validated_.end() && it->second.valid;
}
void MediaCache::candidates(json_t* root, unsigned depth, std::vector<std::pair<std::string, int64_t>>& paths) {
    if (depth > 32) return;
    if (json_is_array(root)) {
        size_t i; json_t* item; json_array_foreach(root, i, item) candidates(item, depth + 1, paths);
    } else if (json_is_object(root)) {
        if (!std::strcmp(str(root, "@type"), "file")) {
            const char* key = str(json_object_get(root, "remote"), "unique_id");
            auto* local = json_object_get(root, "local");
            if (!key[0] || std::strlen(key) > 256 || !json_is_object(local)) return;
            if (json_is_true(json_object_get(local, "is_downloading_completed"))) {
                int64_t size = json_integer_value(json_object_get(root, "size"));
                if (size <= 0) size = json_integer_value(json_object_get(local, "downloaded_size"));
                paths.emplace_back(str(local, "path"), size);
            }
            auto* old = json_object_get(index_, key);
            if (old) paths.emplace_back(str(old, "path"), json_integer_value(json_object_get(old, "size")));
            return;
        }
        const char* key; json_t* value;
        json_object_foreach(root, key, value) candidates(value, depth + 1, paths);
    }
}
bool MediaCache::resolve(json_t* root, bool verify, bool (*proceed)(void*), void* context, bool decorate) {
    performance::Scope measurement(performance::Metric::MediaResolve);
    lockAs(__func__, __LINE__);
    const auto generation = generation_;
    unlockHeld();
    if (verify) {
        std::vector<std::pair<std::string, int64_t>> paths;
        lockAs(__func__, __LINE__);
        candidates(root, 0, paths);
        unlockHeld();
        std::unordered_set<std::string> seen;
        for (const auto& path : paths) {
            auto key = validation_key(path.first.c_str(), path.second);
            if (!seen.insert(key).second) continue;
            if (proceed && !proceed(context)) return false;
            auto now = validation_clock();
            lockAs(__func__, __LINE__);
            auto old = validated_.find(key);
            bool recent = old != validated_.end() && now - old->second.checked < 2000000;
            bool cancelled = generation != generation_;
            unlockHeld();
            if (cancelled) return false;
            if (recent) continue;
            // No cache mutex is held over storage I/O.
            performance::Scope check_measurement(performance::Metric::MediaValidate);
            bool valid = valid_path(path.first.c_str(), path.second);
            check_measurement.finish();
            lockAs(__func__, __LINE__);
            if (generation == generation_) {
                if (validated_.size() >= 16384) validated_.clear();
                validated_[key] = {valid, validation_clock()};
            }
            unlockHeld();
        }
    }
    lockAs(__func__, __LINE__);
    if (generation != generation_) { unlockHeld(); return false; }
    if (decorate) {
        if (!index_) index_ = json_object();
        walk(root, 0);
    }
    unlockHeld();
    return true;
}
void MediaCache::invalidate() {
    lockAs(__func__, __LINE__);
    validated_.clear(); ++generation_;
    clearPending();
    unlockHeld();
}
std::vector<int32_t> MediaCache::enqueue(json_t* root) {
    std::vector<int32_t> displaced;
    lockAs(__func__, __LINE__);
    enqueueFiles(root, 0, displaced);
    unlockHeld();
    return displaced;
}
void MediaCache::enqueueFiles(json_t* root, unsigned depth, std::vector<int32_t>& displaced) {
    if (depth > 32) return;
    if (json_is_array(root)) {
        size_t i; json_t* item; json_array_foreach(root, i, item) enqueueFiles(item, depth + 1, displaced);
    } else if (json_is_object(root)) {
        if (!std::strcmp(str(root, "@type"), "file")) {
            const auto id = (int32_t)json_integer_value(json_object_get(root, "id"));
            if (id <= 0) return; // Old disk snapshots have no usable session ID.
            auto it = pending_.find(id);
            const char* key = str(json_object_get(root, "remote"), "unique_id");
            auto* local = json_object_get(root, "local");
            const bool useful = key[0] && std::strlen(key) <= 256 && json_is_object(local) &&
                std::strlen(str(local, "path")) <= 4096 &&
                (json_is_true(json_object_get(local, "is_downloading_completed")) || json_object_get(index_, key));
            if (!useful) {
                // A deletion/incomplete replacement also cancels an old result.
                if (it != pending_.end()) { json_decref(it->second.file); pending_.erase(it); }
                return;
            }
            // Copy only file fields needed for validation and UI media updates.
            // Never retain a whole message/history or share mutable event JSON.
            auto* copy = json_object();
            for (const char* field : {"@type", "id", "size", "expected_size"})
                json_object_set(copy, field, json_object_get(root, field));
            auto* local_copy = json_object();
            json_object_set_new(local_copy, "path", json_string(str(local, "path")));
            for (const char* field : {"is_downloading_completed", "is_downloading_active", "downloaded_size", "downloaded_prefix_size"})
                json_object_set(local_copy, field, json_object_get(local, field));
            json_object_set_new(copy, "local", local_copy);
            json_object_set_new(copy, "remote", json_pack("{s:s}", "unique_id", key));
            if (it != pending_.end()) {
                if (json_equal(it->second.file, copy)) { json_decref(copy); return; }
                json_decref(it->second.file);
                it->second.file = copy; it->second.serial = ++next_serial_; it->second.ready = false;
            } else {
                if (pending_.size() >= pending_limit) {
                    // Only preview work can be displaced; TD events are untouched.
                    auto oldest = pending_.begin();
                    for (auto p = pending_.begin(); p != pending_.end(); ++p)
                        if (p->second.serial < oldest->second.serial) oldest = p;
                    displaced.push_back(oldest->first);
                    json_decref(oldest->second.file); pending_.erase(oldest);
                }
                pending_.emplace(id, Pending{copy, ++next_serial_, validation_clock()});
            }
            return;
        }
        const char* key; json_t* value;
        json_object_foreach(root, key, value) enqueueFiles(value, depth + 1, displaced);
    }
}
void MediaCache::prioritize(int32_t id) {
    if (id<=0) return;
    lockAs(__func__, __LINE__);
    auto it=pending_.find(id);
    if (it!=pending_.end()) it->second.priority=true;
    unlockHeld();
}
bool MediaCache::awaiting(int32_t id) {
    if (id<=0) return false;
    lockAs(__func__, __LINE__);
    auto it=pending_.find(id);
    bool result=false;
    if (it!=pending_.end() && !it->second.ready && validation_clock()-it->second.queued < 5000000) {
        // Only an indexed copy can save the download; a stuck check never blocks it for long.
        const char* key=str(json_object_get(it->second.file,"remote"),"unique_id");
        auto* local=json_object_get(it->second.file,"local");
        result=key[0] && index_ && json_object_get(index_,key) &&
            !json_is_true(json_object_get(local,"is_downloading_completed"));
    }
    unlockHeld();
    return result;
}
bool MediaCache::validateNext(bool (*proceed)(void*), void* context) {
    if (proceed && !proceed(context)) return false;
    lockAs(__func__, __LINE__);
    auto oldest=pending_.end(), foreground=pending_.end(), background=pending_.end();
    for (auto it=pending_.begin();it!=pending_.end();++it) {
        if (it->second.ready || it->second.working) continue;
        if (oldest==pending_.end() || it->second.serial<oldest->second.serial) oldest=it;
        auto& lane=it->second.priority ? foreground : background;
        if (lane==pending_.end() || it->second.serial<lane->second.serial) lane=it;
    }
    auto next=foreground!=pending_.end() && (priority_turn_%4!=3 || background==pending_.end()) ? foreground :
        background!=pending_.end() ? background : oldest;
    if (next == pending_.end()) { unlockHeld(); return false; }
    ++priority_turn_;
    const auto id = next->first;
    const auto serial = next->second.serial;
    const auto generation = generation_;
    auto* file = json_deep_copy(next->second.file);
    next->second.working = true;
    unlockHeld();
    // Includes stat, but holds neither the event queue nor the cache mutex.
    const bool ok = resolve(file, true, proceed, context, false);
    lockAs(__func__, __LINE__);
    auto it = pending_.find(id);
    if (it != pending_.end() && generation == generation_) {
        it->second.working = false;
        if (ok && it->second.serial == serial) {
            if (!index_) index_ = json_object();
            walk(file, 0);
            json_decref(it->second.file); it->second.file = file; file = nullptr;
            it->second.ready = true;
        }
    }
    unlockHeld();
    if (file) json_decref(file);
    return true;
}
json_t* MediaCache::takeValidated() {
    lockAs(__func__, __LINE__);
    auto foreground=pending_.end(),background=pending_.end();
    for (auto it=pending_.begin();it!=pending_.end();++it) {
        if (!it->second.ready) continue;
        auto& lane=it->second.priority ? foreground : background;
        if (lane==pending_.end() || it->second.serial<lane->second.serial) lane=it;
    }
    auto next=foreground!=pending_.end() && (ready_turn_%4!=3 || background==pending_.end()) ? foreground : background;
    json_t* file = nullptr;
    if (next != pending_.end()) { ++ready_turn_; file = next->second.file; pending_.erase(next); }
    unlockHeld();
    return file;
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
                bool valid = checked(path, size);
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
            if (old && (!expected || expected == size) && checked(path, size)) {
                json_object_set_new(local, "path", json_string(path));
                json_object_set_new(local, "is_downloading_completed", json_true());
                json_object_set_new(local, "is_downloading_active", json_false());
                json_object_set_new(local, "downloaded_size", json_integer(size));
                json_object_set_new(local, "downloaded_prefix_size", json_integer(size));
            } else if (old && validated_.count(validation_key(path, size))) { json_object_del(index_, key); ++revision_; }
            return;
        }
        const char* key; json_t* value;
        json_object_foreach(root, key, value) walk(value, depth + 1);
    }
}
void MediaCache::flush(bool (*proceed)(void*), void* context) {
    performance::Scope measurement(performance::Metric::MediaFlush);
    if (proceed && !proceed(context)) return;
    lockAs(__func__, __LINE__);
    if (!index_ || revision_ == saved_) { unlockHeld(); return; }
    uint64_t revision = revision_;
    // Entries are replaced as whole objects, never changed after insertion.
    json_t* copy = json_copy(index_);
    unlockHeld();
    if (!copy) return;
    auto result = cache_writer::write(copy, temp_path, 4*1024*1024, proceed, context);
    bool ok = result == cache_writer::Result::Ok;
    json_decref(copy);
    if (ok) ok = std::rename(temp_path, index_path) == 0;
    if (ok) {
        lockAs(__func__, __LINE__); saved_ = revision; unlockHeld();
    } else if (result != cache_writer::Result::Interrupted)
        std::fprintf(stderr, "Media cache: index write failed; existing index retained\n");
}
