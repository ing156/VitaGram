#pragma once
#include <cstdio>
#include <string>

namespace cpu_scheduler {
enum class Role { Ui, Audio, Video, Network, Parser, Storage, Prefetch };
// Register only application-owned threads, never system or library threads.
class Thread {
public:
    explicit Thread(Role role);
    ~Thread();
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
private:
    int id_;
};
class Media {
public:
    explicit Media(Role role);
    ~Media();
    Media(const Media&) = delete;
    Media& operator=(const Media&) = delete;
private:
    Role role_;
};
void init(); // UI, before starting workers; asynchronous CPU3 probe.
void tick(); // UI; samples at most once per second.
bool videoActive(); // Background storage must yield while video is decoding.
// Startup flood: the Parser runs alone on CPU3 (CPU2 without it); other background threads avoid it.
void pinParser(bool pinned);
std::string threadStatus(); // UI; registered application-owned threads by role.
// Performance log, any thread: per-core busy percent, ARM clock and per-thread cores since the last call.
void writeReport(FILE* file);
}
