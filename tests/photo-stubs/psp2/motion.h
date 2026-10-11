#pragma once
struct SceMotionState { struct { float x = 0, y = 0, z = 0; } basicOrientation; };
int sceMotionStartSampling();
int sceMotionStopSampling();
int sceMotionGetState(SceMotionState*);
