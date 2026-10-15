#pragma once
#include <cstddef>
inline int td_pump_budget_us(size_t queued,bool interactive,bool recently_active) {
    if (interactive) return 500;
    if (recently_active || queued<128) return 2000;
    if (queued<512) return 4000;
    if (queued<2000) return 6000;
    return 8000;
}
// foreground: the open chat waits for its first history page or the open video
// downloads; background downloads would only queue more file I/O on TDLib's single thread.
// The hold is bounded: a video refreshes `foreground` every frame until it is downloaded,
// which kept thumbnails and avatars queued for minutes (request_wait up to 116 s).
constexpr long long kTdHoldMaxUs = 5000000;
inline bool td_request_allowed(int priority,bool backlog_exempt,bool congested,bool interactive,bool foreground=false,long long waited_us=0) {
    if ((interactive || foreground) && priority<3 && waited_us<kTdHoldMaxUs) return false;
    return !congested || priority>=3 || backlog_exempt;
}
