#pragma once
#include <cstddef>
constexpr int SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW = 1;
int sceKernelAllocMemBlock(const char*, int, size_t, void*);
int sceKernelGetMemBlockBase(int, void**);
int sceKernelFreeMemBlock(int);
