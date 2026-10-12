#pragma once
#include <atomic>
#include <cstdio>

// Runtime switches for the diagnostic log files under ux0:data/vitagram. All are off by default;
// Start -> Diagnostic logs turns them on one by one. Writers must ask enabled() or open().
namespace diag_log {
enum class Kind : unsigned { Performance, Gif, Upload, Video, Audio, Vault, Count };
constexpr int kCount = static_cast<int>(Kind::Count);
constexpr unsigned kAll = (1u << kCount) - 1;
inline std::atomic<unsigned>& mask() { static std::atomic<unsigned> value{0}; return value; }
inline bool enabled(Kind kind) { return (mask().load(std::memory_order_relaxed) >> static_cast<unsigned>(kind)) & 1u; }
inline void set_mask(unsigned value) { mask().store(value & kAll, std::memory_order_relaxed); }
inline FILE* open(Kind kind, const char* path, const char* mode) { return enabled(kind) ? std::fopen(path, mode) : nullptr; }
}
