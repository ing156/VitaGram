#pragma once
#include <algorithm>
#include <cstdint>

// Pause audio when it leads the latest decoded picture; resume after video catches up.
inline bool video_audio_ahead(int64_t audio_us, int64_t video_us, int64_t frame_step_us, bool was_ahead) {
    if (audio_us < 0 || video_us < 0 || audio_us <= video_us) return false;
    const int64_t threshold = std::max<int64_t>(120000, 3 * std::max<int64_t>(0, std::min<int64_t>(1000000, frame_step_us)));
    return audio_us - video_us > (was_ahead ? threshold / 2 : threshold);
}
