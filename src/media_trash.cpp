#include "media_trash.h"
#include "config.h"
#include "performance.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace media_trash {
namespace {
struct Spec {
    const char* path;
    size_t limit;   // Entries above this flag the directory for the next start.
    bool initial;   // Rotated once when the layout marker is missing.
    int layout;     // 0: flat; N: TDLib stores new files in b<N>/ buckets since layout N, rotated once.
    performance::Metric metric;
};
// TDLib keeps profile photos, thumbnails and stickers under the database
// directory (FileDirType::Secure). Stickers are rotated once for layout 3 only
// (their flat directory made every lookup scan 150+ entries).
const Spec specs[] = {
    {VITAGRAM_DB_DIR "/profile_photos", 2000, true, 2, performance::Metric::DirProfilePhotos},
    {VITAGRAM_DB_DIR "/thumbnails", 2000, true, 2, performance::Metric::DirThumbnails},
    {VITAGRAM_DB_DIR "/temp", 300, true, 0, performance::Metric::DirTemp},
    {VITAGRAM_FILES_DIR "/photos", 2000, true, 2, performance::Metric::DirPhotos},
    {VITAGRAM_FILES_DIR "/temp", 300, true, 0, performance::Metric::DirTemp},
    {VITAGRAM_DB_DIR "/stickers", 3000, false, 3, performance::Metric::DirStickers},
};
constexpr const char* layout_marker = VITAGRAM_DB_DIR "/media-layout-1";
constexpr const char* bucket_marker = VITAGRAM_DB_DIR "/media-layout-2";
constexpr const char* bucket3_marker = VITAGRAM_DB_DIR "/media-layout-3";
constexpr const char* trash_suffix = ".trash";

bool exists(const std::string& path) { struct stat info{}; return stat(path.c_str(), &info) == 0; }
std::string flag_path(const char* path) { return std::string(path) + ".rotate"; }
bool touch(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "wb");
    return file && std::fclose(file) == 0;
}
bool skip(const char* name) { return !std::strcmp(name, ".") || !std::strcmp(name, ".."); }
// Throttling must work with diagnostics compiled out.
int64_t clock_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

int rotate() {
    const bool first = !exists(layout_marker);
    const bool upgrade = !exists(bucket_marker);
    const bool upgrade3 = !first && !exists(bucket3_marker);
    int rotated = 0;
    for (const auto& spec : specs) {
        const auto flag = flag_path(spec.path);
        const bool flagged = exists(flag) || (upgrade && spec.layout == 2) || (upgrade3 && spec.layout == 3);
        if (!(first && spec.initial) && !flagged) continue;
        if (exists(spec.path)) {
            std::string target;
            for (int n = 1; n < 100; ++n) {
                target = std::string(spec.path) + trash_suffix + std::to_string(n);
                if (!exists(target)) break;
            }
            // One lookup in the small parent directory; TDLib recreates the
            // empty directory on its next download.
            const auto started = clock_us();
            if (!exists(target) && std::rename(spec.path, target.c_str()) == 0) {
                ++rotated;
                performance::observe(performance::Metric::MediaRotate, clock_us() - started);
            } else std::fprintf(stderr, "Media trash: cannot rotate %s\n", spec.path);
        }
        std::remove(flag.c_str());
    }
    if (upgrade && !touch(bucket_marker)) std::fprintf(stderr, "Media trash: cannot write bucket marker\n");
    if (!exists(bucket3_marker) && !touch(bucket3_marker)) std::fprintf(stderr, "Media trash: cannot write bucket marker 3\n");
    if (first && !touch(layout_marker)) std::fprintf(stderr, "Media trash: cannot write layout marker\n");
    return rotated;
}

Sweeper::~Sweeper() { if (counting_) closedir(counting_); }

void Sweeper::throttle(int64_t now, int64_t elapsed, int64_t minimum) {
    // Keep the card mostly free for TDLib: at most ~20% duty.
    next_ = now + std::max<int64_t>(minimum, elapsed * 4);
}

bool Sweeper::step(int64_t now) {
    if (phase_ == Phase::Done || now < next_) return false;
    const auto started = clock_us();
    if (phase_ == Phase::Discover) {
        for (const char* parent : {VITAGRAM_DB_DIR, VITAGRAM_FILES_DIR}) {
            DIR* dir = opendir(parent);
            if (!dir) continue;
            while (auto* entry = readdir(dir))
                if (std::strstr(entry->d_name, trash_suffix)) trash_.push_back(std::string(parent) + "/" + entry->d_name);
            closedir(dir);
        }
        phase_ = trash_.empty() ? Phase::Count : Phase::Sweep;
        throttle(now, clock_us() - started, 100000);
        return true;
    }
    if (phase_ == Phase::Sweep) {
        if (trash_.empty()) { phase_ = Phase::Count; return false; }
        const auto& directory = trash_.back();
        if (names_.empty()) {
            failures_ = 0;
            if (DIR* dir = opendir(directory.c_str())) {
                while (names_.size() < 64) {
                    auto* entry = readdir(dir);
                    if (!entry) break;
                    if (!skip(entry->d_name)) names_.push_back(entry->d_name);
                }
                closedir(dir);
            }
            // Remove in readdir order: each removal scans only the entries before it.
            std::reverse(names_.begin(), names_.end());
            if (names_.empty()) {
                if (rmdir(directory.c_str())) std::fprintf(stderr, "Media trash: cannot remove %s\n", directory.c_str());
                trash_.pop_back();
            }
            throttle(now, clock_us() - started, 50000);
            return true;
        }
        const auto path = directory + "/" + names_.back();
        names_.pop_back();
        if (std::remove(path.c_str()) && rmdir(path.c_str())) ++failures_;
        const auto elapsed = clock_us() - started;
        performance::observe(performance::Metric::TrashRemove, elapsed);
        // Removal cost grows with the tombstones left in the directory. A single
        // 100-150 ms removal is normal on this exFAT (the sweep is throttled to
        // ~20% duty), so stop only on a very slow one or three slow ones in a row;
        // the rest is left for a PC.
        slow_streak_ = elapsed > 150000 ? slow_streak_ + 1 : 0;
        if (elapsed > 400000 || slow_streak_ >= 3) {
            std::fprintf(stderr, "Media trash: removal too slow, stopping\n");
            trash_.clear(); names_.clear(); phase_ = Phase::Count;
            throttle(now, elapsed, 100000);
            return true;
        }
        if (names_.empty() && failures_ >= 64) {
            // Nothing removable left (e.g. nested directories): retry next session.
            std::fprintf(stderr, "Media trash: giving up on %s\n", directory.c_str());
            trash_.pop_back();
        }
        throttle(now, elapsed, 200000);
        return true;
    }
    // Count: one pass over each live directory, in short chunks.
    if (spec_ >= sizeof(specs) / sizeof(*specs)) { phase_ = Phase::Done; return false; }
    const auto& spec = specs[spec_];
    if (!counting_) {
        counting_ = opendir(spec.path);
        entries_ = 0;
        if (!counting_) { ++spec_; return false; }
    }
    bool finished = false;
    for (int i = 0; i < 256; ++i) {
        auto* entry = readdir(counting_);
        if (!entry) { finished = true; break; }
        if (!skip(entry->d_name)) ++entries_;
    }
    if (finished) {
        closedir(counting_); counting_ = nullptr;
        performance::observe(spec.metric, (int64_t)entries_);
        if (entries_ > spec.limit && !touch(flag_path(spec.path)))
            std::fprintf(stderr, "Media trash: cannot flag %s\n", spec.path);
        ++spec_;
    }
    throttle(now, clock_us() - started, 100000);
    return true;
}
}
