#include "VitaThreading.h"
#include <psp2/kernel/threadmgr.h>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <memory>

extern "C" int _sceKernelGetThreadContextForVM(SceUID, VitaCpuRegisters*, VitaVfpRegisters*);
extern "C" int sceKernelSuspendThreadForVM(SceUID);
extern "C" int sceKernelResumeThreadForVM(SceUID);
extern "C" SceUID __real_sceKernelCreateThread(const char*, SceKernelThreadEntry, int, SceSize, SceUInt, int, const SceKernelThreadOptParam*);

namespace {
struct ThreadState {
    const void* owner;
    SceUID id;
    ThreadState* next = nullptr;
    unsigned suspended = 0;
    bool captured = false;
    VitaGcRegisters registers{};
};
struct Registry {
    std::mutex mutex;
    ThreadState* threads = nullptr;
    ThreadState* find(const void* owner) {
        for (auto* state = threads; state; state = state->next)
            if (state->owner == owner) return state;
        return nullptr;
    }
};
Registry& registry() {
    // Threads may finish after other C++ statics; registrations are removed individually.
    static auto* value = new Registry;
    return *value;
}
thread_local bool creating_vm_thread = false;
}

extern "C" int vitagram_wk_register_thread(const void* owner) {
    if (!owner) return EINVAL;
    const auto id = sceKernelGetThreadId();
    if (id < 0) return EIO;
    // Never allocate/free under this lock: a paused mutator may own the allocator lock.
    auto state = std::make_unique<ThreadState>(ThreadState{owner, id});
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    if (r.find(owner)) return EALREADY;
    state->next = r.threads; r.threads = state.release(); return 0;
}
extern "C" int vitagram_wk_unregister_thread(const void* owner) {
    std::unique_ptr<ThreadState> removed;
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    for (auto** link = &r.threads; *link; link = &(*link)->next) {
        if ((*link)->owner != owner) continue;
        if ((*link)->suspended) return EBUSY;
        removed.reset(*link); *link = removed->next; break;
    }
    return 0;
}
extern "C" int vitagram_wk_suspend_thread(const void* owner) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto* found = r.find(owner);
    if (!found) return ESRCH;
    auto& state = *found;
    if (state.id == sceKernelGetThreadId()) return EDEADLK;
    if (state.suspended) {
        if (!state.captured) return EIO;
        ++state.suspended; return 0;
    }
    const int status = sceKernelSuspendThreadForVM(state.id);
    if (status < 0) return status;
    VitaCpuRegisters cpu{}; cpu.size = sizeof(cpu);
    VitaVfpRegisters vfp{}; vfp.size = sizeof(vfp);
    const int captured = _sceKernelGetThreadContextForVM(state.id, &cpu, &vfp);
    if (captured < 0) {
        const int resumed = sceKernelResumeThreadForVM(state.id);
        // Keep the outstanding hold visible if rollback itself failed.
        if (resumed < 0) { state.suspended = 1; return resumed; }
        return captured;
    }
    std::memcpy(state.registers.general, cpu.reg, sizeof(cpu.reg));
    std::memcpy(state.registers.floating, vfp.reg, sizeof(vfp.reg));
    state.suspended = 1; state.captured = true; return 0;
}
extern "C" int vitagram_wk_resume_thread(const void* owner) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto* found = r.find(owner);
    if (!found) return ESRCH;
    auto& state = *found;
    if (!state.suspended) return EINVAL;
    if (state.suspended > 1) { --state.suspended; return 0; }
    const int status = sceKernelResumeThreadForVM(state.id);
    if (status < 0) return status;
    state.suspended = 0;
    state.captured = false;
    std::memset(&state.registers, 0, sizeof(state.registers)); return 0;
}
extern "C" int vitagram_wk_copy_registers(const void* owner, VitaGcRegisters* output) {
    if (!output) return EINVAL;
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto* found = r.find(owner);
    if (!found) return ESRCH;
    if (!found->suspended || !found->captured) return EBUSY;
    *output = found->registers; return 0;
}
extern "C" int vitagram_wk_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*entry)(void*), void* data) {
    const bool previous = creating_vm_thread; creating_vm_thread = true;
    const int result = pthread_create(thread, attr, entry, data);
    creating_vm_thread = previous; return result;
}
extern "C" SceUID __wrap_sceKernelCreateThread(const char* name, SceKernelThreadEntry entry, int priority, SceSize stack, SceUInt attr, int affinity, const SceKernelThreadOptParam* options) {
    return __real_sceKernelCreateThread(name, entry, priority, stack,
        attr | (creating_vm_thread ? VITAGRAM_WK_THREAD_VM : 0u), affinity, options);
}
