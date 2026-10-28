#pragma once
#include <cstdint>

namespace media_limits {
// Leave headroom for encrypted-file overhead within Vita's signed 32-bit file offsets.
inline constexpr int64_t video_bytes = 1024LL * 1024 * 1024;
}
