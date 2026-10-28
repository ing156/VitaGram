#include "cpu_scheduler.h"
#include <psp2/kernel/threadmgr.h>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <condition_variable>
#include <thread>
using namespace std::chrono_literals;
static std::atomic<int64_t> clock_us{2000000};
int64_t sceKernelGetSystemTimeWide() { return clock_us; }
int sceKernelGetThreadId() { static std::atomic<int> next{0}; thread_local int id = ++next; return id; }
int sceKernelGetCpuId() { return 0; }
int sceKernelGetThreadInfo(int, SceKernelThreadInfo* info) {
    info->currentPriority = 0x10000100; std::strcpy(info->name, "test"); return 0;
}
int sceKernelCreateThread(const char*, int (*)(SceSize, void*), int, unsigned, unsigned, int, void*) { return -1; }
int sceKernelStartThread(int, unsigned, void*) { return 0; }
int sceKernelDeleteThread(int) { return 0; }
int sceKernelChangeThreadCpuAffinityMask(int, int) { return 0; }
int sceKernelChangeThreadPriority(int, int) { return 0; }
int scePowerGetArmClockFrequency() { return 500; }
int sceKernelGetSystemInfo(SceKernelSystemInfo* info) {
    for (auto& cpu : info->cpuInfo) cpu.idleClock = clock_us / 2;
    return 0;
}
static FILE* slow_file;
static std::mutex io_mutex;
static std::condition_variable io_changed;
static bool entered = false, released = false;
extern "C" int __wrap_fprintf(FILE* file, const char* format, ...) {
    if (file == slow_file) {
        std::unique_lock<std::mutex> lock(io_mutex);
        entered = true; io_changed.notify_all();
        io_changed.wait(lock, [] { return released; });
    }
    va_list args; va_start(args, format);
    const int result = std::vfprintf(file, format, args);
    va_end(args); return result;
}
static std::string contents(FILE* file) {
    std::rewind(file); std::string result; char line[512];
    while (std::fgets(line, sizeof(line), file)) result += line;
    return result;
}
int main() {
    cpu_scheduler::Thread ui(cpu_scheduler::Role::Ui);
    cpu_scheduler::tick(); clock_us += 1000000; cpu_scheduler::tick();
    slow_file = std::tmpfile(); assert(slow_file);
    std::thread report([] { cpu_scheduler::writeReport(slow_file); });
    {
        std::unique_lock<std::mutex> lock(io_mutex);
        assert(io_changed.wait_for(lock, 2s, [] { return entered; }));
    }
    auto controls = std::async(std::launch::async, [] {
        clock_us += 1000000; cpu_scheduler::tick();
        assert(!cpu_scheduler::videoActive());
        cpu_scheduler::Media video(cpu_scheduler::Role::Video);
        assert(cpu_scheduler::videoActive());
        cpu_scheduler::pinParser(true);
        assert(cpu_scheduler::threadStatus().find("UI:1") != std::string::npos);
    });
    const bool responsive = controls.wait_for(500ms) == std::future_status::ready;
    { std::lock_guard<std::mutex> lock(io_mutex); released = true; io_changed.notify_all(); }
    report.join(); controls.get();
    assert(responsive); // Old writeReport held the scheduler mutex throughout blocked I/O.
    const auto first = contents(slow_file);
    assert(first.find("cpu_cores samples=1") != std::string::npos);
    assert(first.find("seconds=2,0,0,0") != std::string::npos);
    FILE* next = std::tmpfile(); assert(next);
    cpu_scheduler::writeReport(next);
    const auto second = contents(next);
    assert(second.find("cpu_cores samples=1") != std::string::npos); // Tick during I/O belongs to the next report.
    assert(second.find("seconds=1,0,0,0") != std::string::npos);
    cpu_scheduler::writeReport(next);
    assert(contents(next).find("cpu_cores samples=0") != std::string::npos);
    std::fclose(next); std::fclose(slow_file);
    std::puts("CPU report: stalled log output leaves UI ticks and media transitions responsive; interval counters preserved");
}
