#pragma once
#include <stdint.h>
#include <pthread.h>

// ABI from the SceThreadmgr ForVM exports; register words remain opaque GC roots.
typedef struct { uint32_t size, cpsr, reg[16], tpidrurw, teehbr, sb, st; } VitaCpuRegisters;
typedef struct { uint32_t size, fpscr, reg[64]; } VitaVfpRegisters;
typedef struct { uint32_t general[16], floating[64]; } VitaGcRegisters;
#define VITAGRAM_WK_THREAD_VM 0x04000000u

#ifdef __cplusplus
extern "C" {
#endif
int vitagram_wk_register_thread(const void*);
int vitagram_wk_unregister_thread(const void*);
int vitagram_wk_suspend_thread(const void*);
int vitagram_wk_resume_thread(const void*);
int vitagram_wk_copy_registers(const void*, VitaGcRegisters*);
int vitagram_wk_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
#ifdef __cplusplus
}
static_assert(sizeof(VitaCpuRegisters) == 0x58);
static_assert(sizeof(VitaVfpRegisters) == 0x108);
static_assert(sizeof(VitaGcRegisters) == 320);
#endif
