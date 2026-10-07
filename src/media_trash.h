#pragma once
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <string>
#include <vector>

// TDLib runs without its file database, so media downloaded in earlier
// sessions is unknown to it and lands again in the same flat directories.
// On the Vita's exFAT every open/stat/create scans the directory linearly:
// with thousands of entries one avatar create took ~0.8 s on TDLib's only
// thread. Bloated directories are swapped for empty ones before TDLib starts;
// the old copies are emptied later in small throttled steps.
namespace media_trash {
// Before TDLib starts only. Returns the number of rotated directories.
int rotate();

// Storage thread only. Each step does at most one small piece of I/O.
class Sweeper {
public:
    ~Sweeper();
    // Returns true when it touched the card; false when idle or throttled.
    bool step(int64_t now);
    bool done() const { return phase_ == Phase::Done; }
private:
    enum class Phase { Discover, Sweep, Count, Done };
    Phase phase_ = Phase::Discover;
    int64_t next_ = 0;
    std::vector<std::string> trash_, names_;
    size_t failures_ = 0;
    int slow_streak_ = 0;
    size_t spec_ = 0, entries_ = 0;
    DIR* counting_ = nullptr;
    void throttle(int64_t now, int64_t elapsed, int64_t minimum);
};
}
