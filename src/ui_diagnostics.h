#pragma once
#include "performance.h"
#include <atomic>
#include <cstdio>

// What the UI thread is doing right now, for the performance log ("ui_active ..."). A frame that
// never ends records no slow_event, so a stuck UI thread is otherwise invisible in the log.
// Only the UI thread publishes; the log worker reads one coherent value.
namespace ui_diagnostics {
enum class Phase : unsigned {
    Idle, Frame, Pump, Input, Media, Draw, Swap,
    VaultScreen, VaultDerive, VaultScan, VaultSeal, VaultVerify, VaultMigration,
};
constexpr uint64_t time_mask = (uint64_t{1} << 56) - 1;
inline std::atomic<uint64_t>& active() { static std::atomic<uint64_t> value{0}; return value; }
inline const char* name(unsigned phase) {
    constexpr const char* names[] = {"idle", "frame", "pump", "input", "media", "draw", "swap",
                                    "vault_screen", "vault_derive", "vault_scan", "vault_seal", "vault_verify", "vault_migration"};
    return phase < sizeof(names)/sizeof(*names) ? names[phase] : "unknown";
}
inline uint64_t enter(Phase phase) {
    if (!performance::enabled) return 0;
    return active().exchange((uint64_t(phase) << 56) | (performance::now() & time_mask), std::memory_order_relaxed);
}
inline void leave(uint64_t previous) {
    if (performance::enabled) active().store(previous, std::memory_order_relaxed);
}
// Scoped phase; to() switches it without nesting, the destructor restores the outer one.
class Stage {
public:
    explicit Stage(Phase phase) : previous_(enter(phase)) {}
    ~Stage() { leave(previous_); }
    void to(Phase phase) { enter(phase); }
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
private:
    uint64_t previous_;
};
inline void write(FILE* file) {
    const auto state = active().load(std::memory_order_relaxed);
    const unsigned phase = state >> 56;
    const auto age = phase ? (performance::now() - (state & time_mask)) & time_mask : 0;
    std::fprintf(file, "ui_active phase=%s age_us=%llu\n", name(phase), (unsigned long long)age);
}
}
