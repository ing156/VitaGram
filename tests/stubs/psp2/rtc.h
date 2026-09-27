#pragma once
#include <ctime>
struct SceDateTime { unsigned day, month, year, hour, minute; };
struct SceRtcTick { unsigned long long tick; };
inline int sceRtcSetTime_t(SceDateTime*, time_t) { return -1; }
inline int sceRtcGetTick(SceDateTime*, SceRtcTick*) { return -1; }
inline int sceRtcConvertUtcToLocalTime(SceRtcTick*, SceRtcTick*) { return -1; }
inline int sceRtcSetTick(SceDateTime*, SceRtcTick*) { return -1; }
