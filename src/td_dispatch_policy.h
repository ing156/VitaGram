#pragma once
#include <cstddef>
inline int td_pump_budget_us(size_t queued,bool interactive,bool recently_active) {
    if (interactive) return 500;
    if (recently_active || queued<128) return 2000;
    if (queued<512) return 4000;
    if (queued<2000) return 6000;
    return 8000;
}
// foreground: the open chat waits for its first history page; background
// downloads would only queue more file I/O on TDLib's single thread.
inline bool td_request_allowed(int priority,bool backlog_exempt,bool congested,bool interactive,bool foreground=false) {
    if ((interactive || foreground) && priority<3) return false;
    return !congested || priority>=3 || backlog_exempt;
}
