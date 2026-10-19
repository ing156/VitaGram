#pragma once
struct SceNetInitParam { void* memory; int size, flags; };
int sceNetInit(SceNetInitParam*);
int sceNetTerm();
