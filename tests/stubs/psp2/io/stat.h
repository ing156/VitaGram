#pragma once
#include <cstdint>
struct SceIoStat { int64_t st_size; };
int sceIoGetstat(const char*, SceIoStat*);
int sceIoMkdir(const char*, int);
