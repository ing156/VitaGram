#pragma once
#include <cstddef>
// Until the first live chat list arrives the UI only draws the cached list, and every queued
// startup update delays the live one: the flood gets most of the frame instead of 2-8 ms.
// 25 ms drops the static list to ~30 fps; input switches back to the interactive budget.
constexpr int kTdStartupPumpUs = 25000;
inline int td_pump_budget_us(size_t queued,bool interactive,bool recently_active,bool startup=false) {
    if (interactive) return 500;
    if (recently_active) return 2000;
    if (startup && queued>=32) return kTdStartupPumpUs;
    if (queued<128) return 2000;
    if (queued<512) return 4000;
    if (queued<2000) return 6000;
    return 8000;
}
// foreground: the open chat waits for its first history page or the open video
// downloads; background downloads would only queue more file I/O on TDLib's single thread.
// The hold is bounded: a video refreshes `foreground` every frame until it is downloaded,
// which kept thumbnails and avatars queued for minutes (request_wait up to 116 s).
constexpr long long kTdHoldMaxUs = 5000000;
// video_hold: an open video post shows nothing else, so its download keeps the link for as long as it loads.
inline bool td_request_allowed(int priority,bool backlog_exempt,bool congested,bool interactive,bool foreground=false,long long waited_us=0,bool video_hold=false) {
    if ((interactive || foreground) && priority<3 && (video_hold || waited_us<kTdHoldMaxUs)) return false;
    return !congested || priority>=3 || backlog_exempt;
}
