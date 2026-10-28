#include "webview/VitaThreading.h"
#include <psp2/kernel/threadmgr.h>
#include <cassert>
#include <cerrno>
#include <cstdio>

extern "C" SceUID __wrap_sceKernelCreateThread(const char*, SceKernelThreadEntry, int, SceSize, SceUInt, int, const SceKernelThreadOptParam*);
namespace {
int current = 41, stopped = 0, resumed = 0;
int stopResult = 0, resumeResult = 0, captureResult = 0, createResult = 0;
unsigned lastFlags = 0;
}
extern "C" SceUID sceKernelGetThreadId() { return current; }
extern "C" int sceKernelSuspendThreadForVM(SceUID id) { assert(id == 41); ++stopped; return stopResult; }
extern "C" int sceKernelResumeThreadForVM(SceUID id) { assert(id == 41); ++resumed; return resumeResult; }
extern "C" int _sceKernelGetThreadContextForVM(SceUID id, VitaCpuRegisters* cpu, VitaVfpRegisters* vfp) {
    assert(id == 41 && cpu->size == 0x58 && vfp->size == 0x108);
    for (unsigned i = 0; i < 16; ++i) cpu->reg[i] = 0x1000 + i;
    for (unsigned i = 0; i < 64; ++i) vfp->reg[i] = 0x2000 + i;
    return captureResult;
}
extern "C" SceUID __real_sceKernelCreateThread(const char*, SceKernelThreadEntry, int, SceSize, SceUInt flags, int, const SceKernelThreadOptParam*) {
    lastFlags = flags; return 123;
}
extern "C" int __wrap_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*) {
    __wrap_sceKernelCreateThread("test", nullptr, 0, 0, 0x10, 0, nullptr);
    return createResult;
}
int main() {
    int owner = 0, missing = 0;
    VitaGcRegisters registers{};
    assert(vitagram_wk_register_thread(nullptr) == EINVAL);
    assert(vitagram_wk_suspend_thread(&missing) == ESRCH);
    assert(!vitagram_wk_register_thread(&owner));
    assert(vitagram_wk_register_thread(&owner) == EALREADY);
    assert(vitagram_wk_suspend_thread(&owner) == EDEADLK);
    current = 42;
    assert(vitagram_wk_copy_registers(&owner, &registers) == EBUSY);
    stopResult = -11;
    assert(vitagram_wk_suspend_thread(&owner) == -11);
    assert(vitagram_wk_resume_thread(&owner) == EINVAL);
    stopResult = 2; // Native success can be a positive suspend count.
    assert(!vitagram_wk_suspend_thread(&owner));
    assert(!vitagram_wk_suspend_thread(&owner));
    assert(stopped == 2);
    assert(vitagram_wk_unregister_thread(&owner) == EBUSY);
    assert(vitagram_wk_copy_registers(&owner, nullptr) == EINVAL);
    assert(!vitagram_wk_copy_registers(&owner, &registers));
    for (unsigned i = 0; i < 16; ++i) assert(registers.general[i] == 0x1000 + i);
    for (unsigned i = 0; i < 64; ++i) assert(registers.floating[i] == 0x2000 + i);
    assert(!vitagram_wk_resume_thread(&owner) && resumed == 0);
    resumeResult = -12;
    assert(vitagram_wk_resume_thread(&owner) == -12);
    assert(!vitagram_wk_copy_registers(&owner, &registers));
    resumeResult = 1;
    assert(!vitagram_wk_resume_thread(&owner));
    assert(vitagram_wk_copy_registers(&owner, &registers) == EBUSY);
    captureResult = -13;
    assert(vitagram_wk_suspend_thread(&owner) == -13);
    assert(vitagram_wk_copy_registers(&owner, &registers) == EBUSY);
    assert(vitagram_wk_resume_thread(&owner) == EINVAL);
    resumeResult = -14;
    assert(vitagram_wk_suspend_thread(&owner) == -14);
    assert(vitagram_wk_unregister_thread(&owner) == EBUSY);
    assert(vitagram_wk_copy_registers(&owner, &registers) == EBUSY);
    assert(vitagram_wk_suspend_thread(&owner) == EIO);
    resumeResult = 0;
    assert(!vitagram_wk_resume_thread(&owner));
    assert(!vitagram_wk_unregister_thread(&owner));
    assert(!vitagram_wk_unregister_thread(&missing));
    assert(vitagram_wk_copy_registers(&owner, &registers) == ESRCH);
    int first = 0, middle = 0, last = 0;
    current = 41;
    assert(!vitagram_wk_register_thread(&first));
    assert(!vitagram_wk_register_thread(&middle));
    assert(!vitagram_wk_register_thread(&last));
    assert(!vitagram_wk_unregister_thread(&middle));
    current = 42; captureResult = 0;
    assert(!vitagram_wk_suspend_thread(&first));
    assert(!vitagram_wk_suspend_thread(&last));
    assert(vitagram_wk_suspend_thread(&middle) == ESRCH);
    assert(!vitagram_wk_resume_thread(&first));
    assert(!vitagram_wk_resume_thread(&last));
    assert(!vitagram_wk_unregister_thread(&last));
    assert(!vitagram_wk_unregister_thread(&first));
    pthread_t thread{};
    createResult = EAGAIN;
    assert(vitagram_wk_pthread_create(&thread, nullptr, nullptr, nullptr) == EAGAIN);
    assert(lastFlags == (0x10 | VITAGRAM_WK_THREAD_VM));
    __wrap_sceKernelCreateThread("ordinary", nullptr, 0, 0, 0x20, 0, nullptr);
    assert(lastFlags == 0x20); // Scope restored even after failed creation.
    std::puts("WebKit Vita thread broker: OK");
}
