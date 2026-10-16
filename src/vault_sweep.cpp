#include "vault_sweep.h"
#include "config.h"
#include "cpu_scheduler.h"
#include "vault.h"
#include "download_stream.h"
#include <dirent.h>
#include <psp2/kernel/threadmgr.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

namespace vault_sweep {
namespace {
bool ends_with(const std::string& text, const char* suffix) {
    size_t n = std::strlen(suffix);
    return text.size() >= n && !text.compare(text.size() - n, n, suffix);
}

std::string normalized(std::string path) {
    auto colon = path.find(':');
    if (colon != std::string::npos && path.compare(colon + 1, 1, "/") == 0 && path.compare(colon + 1, 2, "//") != 0)
        path.erase(colon + 1, 1); // TDLib may return ux0:/data instead of ux0:data
    return path;
}

bool has_prefix(const std::string& path, const std::string& prefix) { return !path.compare(0, prefix.size(), prefix); }

// `errors` counts what could not be listed (unreadable directory or entry, too deep): a migration
// that could not see a file must not be reported as complete. Links are never followed.
void walk(const std::string& directory, bool top_of_db, std::vector<std::string>& out, size_t& errors, int depth = 0) {
    // The directory itself is checked too: a link put in place of a root is not followed.
    struct stat self{};
    if (lstat(directory.c_str(), &self) != 0) return;               // absent: nothing to convert
    if (!S_ISDIR(self.st_mode)) { ++errors; return; }                // link or file instead of a directory
    DIR* dir = opendir(directory.c_str());
    if (!dir) { ++errors; return; }                                  // exists but cannot be read
    if (depth > 16) { ++errors; closedir(dir); return; }
    errno = 0;
    while (auto* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string path = directory + "/" + name;
        struct stat info{};
        if (lstat(path.c_str(), &info)) { ++errors; continue; }
        if (S_ISLNK(info.st_mode)) continue;
        if (S_ISDIR(info.st_mode)) {
            if (!ends_with(name, ".trash")) walk(path, false, out, errors, depth + 1); // trash is being emptied anyway
        } else if (S_ISREG(info.st_mode) && info.st_size > 0) {
            if (ends_with(name, ".vgs") || ends_with(name, ".tmp")) { if (ends_with(name, ".vgs")) std::remove(path.c_str()); continue; }
            // Database root: only the JSON caches are ours, the rest belongs to TDLib.
            if (top_of_db && !ends_with(name, ".json")) continue;
            out.push_back(path);
        }
        errno = 0;
    }
    if (errno) ++errors; // readdir stopped on an error, not at the end
    closedir(dir);
}
}

static std::atomic<int> g_pauses{0};
static std::atomic<int> g_background_busy{0};

bool is_db_cache(const std::string& raw) {
    std::string path = normalized(raw);
    const std::string db = std::string(VITAGRAM_DB_DIR) + "/";
    if (!has_prefix(path, db) || !ends_with(path, ".json")) return false;
    // Chat histories of the UI cache live one file per chat in ui-history/.
    const size_t start = has_prefix(path, db + "ui-history/") ? db.size() + 11 : db.size();
    return path.find('/', start) == std::string::npos;
}

bool is_user_file(const std::string& raw) {
    std::string path = normalized(raw);
    if (path.find("..") != std::string::npos || ends_with(path, ".vgs") || ends_with(path, ".tmp")) return false;
    const std::string files = std::string(VITAGRAM_FILES_DIR) + "/", db = std::string(VITAGRAM_DB_DIR) + "/";
    if (has_prefix(path, files)) return true;
    if (has_prefix(path, db)) {
        std::string rest = path.substr(db.size());
        return rest.find('/') != std::string::npos || ends_with(rest, ".json");
    }
    return false;
}

std::vector<std::string> list_user_files(size_t* listing_errors) {
    std::vector<std::string> files;
    size_t errors = 0;
    walk(VITAGRAM_FILES_DIR, false, files, errors);
    walk(VITAGRAM_DB_DIR, true, files, errors);
    // Derived portrait videos live next to the settings; only regular files, never links.
    if (DIR* dir = opendir("ux0:data/vitagram")) {
        errno = 0;
        while (auto* entry = readdir(dir)) {
            std::string name = entry->d_name, path = "ux0:data/vitagram/" + name;
            struct stat info{};
            if (name.compare(0, 9, "portrait-") == 0 && ends_with(name, ".mp4")) {
                if (lstat(path.c_str(), &info) != 0) ++errors;
                else if (S_ISREG(info.st_mode)) files.push_back(path);
            }
            if (ends_with(name, ".mp4.part")) std::remove(path.c_str());
            errno = 0;
        }
        if (errno) ++errors;
        closedir(dir);
    }
    if (listing_errors) *listing_errors = errors;
    return files;
}

SealerPause::SealerPause() {
    g_pauses.fetch_add(1);
    // The Sealer marks itself busy before it checks for a pause, so once it is idle here it
    // cannot start a file until the pause ends.
    while (g_background_busy.load()) sceKernelDelayThread(5000);
}

SealerPause::~SealerPause() { g_pauses.fetch_sub(1); }

bool sealer_paused() { return g_pauses.load() > 0; }

Outcome convert_all(bool to_sealed, Proceed proceed, void* context, Report report, void* report_context) {
    Outcome outcome;
    auto files = list_user_files(&outcome.unlisted);
    outcome.total = files.size();
    size_t done = 0;
    for (const auto& path : files) {
        if (proceed && !proceed(context)) { if (report) report(report_context, done, outcome.total); return outcome; }
        // Without an authenticated vault state, a cache that is not already vault-sealed may have been
        // put there from outside: sealing it would make it look trusted. Caches are rebuilt, so drop it.
        if (to_sealed && vault::state_unauthenticated() && is_db_cache(path) && !vault::is_vault_sealed(path)) {
            if (std::remove(path.c_str()) == 0) ++outcome.converted; else ++outcome.failed;
            ++done;
            continue;
        }
        auto result = to_sealed ? vault::seal_file(path, proceed, context) : vault::unseal_file(path, proceed, context);
        if (result == vault::Convert::Interrupted) return outcome;
        if (result == vault::Convert::Done) ++outcome.converted;
        else if (result == vault::Convert::Failed) ++outcome.failed;
        ++done;
        if (report && (done % 8 == 0 || done == outcome.total)) report(report_context, done, outcome.total);
    }
    outcome.completed = true;
    return outcome;
}

void Sealer::start() {
    if (started_ || !vault::sealing()) return;
    stopping_ = false;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 256 * 1024);
    started_ = pthread_create(&thread_, &attributes, entry, this) == 0;
    pthread_attr_destroy(&attributes);
}

void Sealer::stop() {
    if (!started_) return;
    stopping_ = true;
    pthread_join(thread_, nullptr);
    started_ = false;
}

void Sealer::enqueue(const std::string& path) {
    if (!started_ || path.empty() || !is_user_file(path)) return;
    pthread_mutex_lock(&mutex_);
    if (known_.size() > 20000) known_.clear();
    if (known_.insert(path).second) queue_.emplace_back(path, 0);
    pthread_mutex_unlock(&mutex_);
}

size_t Sealer::pending() {
    pthread_mutex_lock(&mutex_);
    size_t n = queue_.size();
    pthread_mutex_unlock(&mutex_);
    return n;
}

void* Sealer::entry(void* self) {
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Storage);
    static_cast<Sealer*>(self)->run();
    return nullptr;
}

void Sealer::run() {
    // Called about every 64 KiB while a file is converted. Converting must never compete with
    // video playback for the card and the CPU: give way at once and finish later.
    auto proceed = [](void* opaque) -> bool {
        auto* self = static_cast<Sealer*>(opaque);
        if (self->stopping_ || sealer_paused() || cpu_scheduler::videoActive() || download_stream::reading()) return false;
        sceKernelDelayThread(1500); // leaves room for the decoder threads even when idle
        return true;
    };
    while (!stopping_) {
        std::pair<std::string, int> item;
        pthread_mutex_lock(&mutex_);
        bool have = !queue_.empty();
        if (have) { item = std::move(queue_.front()); queue_.pop_front(); }
        pthread_mutex_unlock(&mutex_);
        if (!have) { sceKernelDelayThread(200000); continue; }
        // Let a freshly completed file finish being used by whoever reported it.
        if (item.second == 0) sceKernelDelayThread(300000);
        // Wait for playback to end, plus a pause so the next video does not start into a rewrite.
        while (!stopping_ && (sealer_paused() || cpu_scheduler::videoActive() || download_stream::reading())) sceKernelDelayThread(200000);
        if (stopping_) break;
        g_background_busy.fetch_add(1);
        auto result = sealer_paused() ? vault::Convert::Interrupted : vault::seal_file(item.first, proceed, this);
        g_background_busy.fetch_sub(1);
        if (result == vault::Convert::Interrupted && !stopping_) {
            pthread_mutex_lock(&mutex_);
            queue_.emplace_back(item.first, item.second); // not a failure: try again after playback
            pthread_mutex_unlock(&mutex_);
            sceKernelDelayThread(500000);
        } else if (result == vault::Convert::Failed && item.second < 3 && !stopping_) {
            // Most likely still open elsewhere; try again a little later.
            sceKernelDelayThread(500000);
            pthread_mutex_lock(&mutex_);
            queue_.emplace_back(item.first, item.second + 1);
            pthread_mutex_unlock(&mutex_);
        }
        sceKernelDelayThread(2000);
    }
}
}
