#pragma once
#include <string>

namespace cpu_scheduler {
enum class Role { Ui, Audio, Video, Network, Parser };
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
std::string status(); // UI; measured system load, not an estimated app percentage.
std::string threadStatus(); // UI; registered application-owned threads by role.
}
