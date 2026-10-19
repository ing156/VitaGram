#pragma once
#include <fcntl.h>
#include <cstdio>
using SceUID = int;
using SceOff = long long;
constexpr int SCE_O_RDONLY = O_RDONLY;
constexpr int SCE_SEEK_END = SEEK_END, SCE_SEEK_SET = SEEK_SET;
int sceIoOpen(const char*, int, int);
SceOff sceIoLseek(int, SceOff, int);
int sceIoRead(int, void*, unsigned);
int sceIoClose(int);
