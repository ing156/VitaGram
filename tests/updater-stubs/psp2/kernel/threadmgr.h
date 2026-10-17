#pragma once
using SceSize = unsigned;
using SceUID = int;
int sceKernelCreateThread(const char*, int (*)(SceSize, void*), int, unsigned, int, int, void*);
int sceKernelStartThread(SceUID, SceSize, void*);
int sceKernelWaitThreadEnd(SceUID, void*, void*);
int sceKernelDeleteThread(SceUID);
int sceKernelDelayThread(unsigned);
