#pragma once
#include <atomic>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

// Priority ceiling for locks the UI thread waits on. A background holder (Storage, Parser)
// preempted on its core by a busier thread kept the UI waiting 130-190 ms for a short critical
// section. While a thread holds such a lock it runs at the UI priority. Nested locks raise once.
namespace priority_ceiling {
inline std::atomic<int> ui_priority{0}; // Set by cpu_scheduler::init; 0 (host tests) disables it.
namespace detail {
inline thread_local int depth = 0;
inline thread_local int saved = 0;
}
inline void enter() {
    if (detail::depth++) return;
    detail::saved = 0;
#ifdef __vita__
    const int ceiling = ui_priority.load(std::memory_order_relaxed);
    if (!ceiling) return;
    const int current = sceKernelGetThreadCurrentPriority();
    // A smaller value is a higher priority; the UI and audio threads are never lowered.
    if (current > ceiling && sceKernelChangeThreadPriority(sceKernelGetThreadId(), ceiling) >= 0)
        detail::saved = current;
#endif
}
inline void leave() {
    if (detail::depth <= 0 || --detail::depth) return;
#ifdef __vita__
    if (detail::saved) sceKernelChangeThreadPriority(sceKernelGetThreadId(), detail::saved);
#endif
    detail::saved = 0;
}
}
