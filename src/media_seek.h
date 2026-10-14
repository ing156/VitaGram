#pragma once
#include <algorithm>
#include <cstdint>

namespace media_seek {
struct TimeSelection {
    int64_t seconds = 0, maximum = 0;
    int field = 1; // Hours, minutes, seconds.
    void begin(int64_t position_us, int64_t duration_us, int64_t end_margin_us) {
        maximum = std::max<int64_t>(0, duration_us - end_margin_us) / 1000000;
        seconds = std::max<int64_t>(0, std::min(maximum, position_us / 1000000));
        field = duration_us < 60000000 ? 2 : 1;
    }
    void select(int direction) { field = (field + direction + 3) % 3; }
    void adjust(int direction) {
        const int step = field == 0 ? 3600 : field == 1 ? 60 : 1;
        seconds = std::max<int64_t>(0, std::min(maximum, seconds + direction * int64_t(step)));
    }
    int64_t targetUs() const { return seconds * 1000000; }
};
// Decode every reference frame from the preceding keyframe, but present only
// the frame containing the selected instant (or the first frame after it).
inline bool before_target(int64_t start_us, int64_t duration_us, int64_t target_us) {
    return target_us > 0 && start_us < target_us && duration_us <= target_us - start_us;
}
}
