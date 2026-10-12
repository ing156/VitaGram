#pragma once
#include <pthread.h>
#include <atomic>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

// Moves files between plain and sealed form.
//   * convert_all: the one-off migration when the vault is switched on or off. It runs before
//     TDLib starts, so no file is changed while TDLib may still be writing it.
//   * Sealer: a background thread that seals media as soon as TDLib reports it fully downloaded.
// Which files count as user data: everything under the files directory, every subdirectory of the
// database directory (thumbnails, stickers, profile photos), the JSON caches, and the derived
// portrait videos. TDLib's own database files in the database root are not touched (TDLib
// encrypts those itself with the vault-derived key).
namespace vault_sweep {
bool is_user_file(const std::string& path);
// A JSON cache directly in the database directory (ui-cache.json and the like).
bool is_db_cache(const std::string& path);
// Absolute list of user files; stale ".vgs" conversion leftovers are deleted on the way. Links are
// not followed. `listing_errors` (optional) receives how many directories or entries could not be read.
std::vector<std::string> list_user_files(size_t* listing_errors = nullptr);

// `completed`: the loop ran to the end (not interrupted). `clean()`: in addition every file was
// listed and converted - only then may the migration be considered done (and, when switching the
// vault off, the key be discarded).
struct Outcome {
    size_t total = 0, converted = 0, failed = 0, unlisted = 0;
    bool completed = false;
    bool clean() const { return completed && failed == 0 && unlisted == 0; }
};
using Proceed = bool (*)(void*);
using Report = void (*)(void*, size_t done, size_t total);
// Converts every user file to sealed (to_sealed) or back to plain. Stops early when `proceed`
// returns false; the vault keeps a "migration pending" flag so the next start resumes.
Outcome convert_all(bool to_sealed, Proceed proceed, void* context, Report report, void* report_context);

class Sealer {
public:
    ~Sealer() { stop(); }
    // Starts only while the vault is sealing; harmless otherwise.
    void start();
    void stop();
    // Any thread. Cheap: duplicates and non-user files are dropped.
    void enqueue(const std::string& path);
    size_t pending();
private:
    static void* entry(void* self);
    void run();
    pthread_t thread_{};
    bool started_ = false;
    std::atomic<bool> stopping_{false};
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::deque<std::pair<std::string, int>> queue_; // path, attempts
    std::unordered_set<std::string> known_;          // paths already handled this session
};
}
