#pragma once
#include <cstdint>
#include <sys/stat.h>
struct SceIoStat { int64_t st_size; mode_t st_mode; };
#define SCE_S_ISDIR(mode) S_ISDIR(mode)
int sceIoGetstat(const char*, SceIoStat*);
int sceIoMkdir(const char*, int);
