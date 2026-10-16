#pragma once
#include <cstdint>

// Resolve one gesture into either a letter or its alternate character, never both.
struct KeyboardHold {
    enum class Result { None, Short, Long };
    // HeliBoard Defaults.kt uses PREF_KEY_LONGPRESS_TIMEOUT = 300 ms.
    // The UI opens choices on Long and commits the chosen symbol on release.
    static constexpr uint64_t delay = 300000;
    bool active = false, fired = false;
    uint64_t since = 0;

    void start(uint64_t now) { active = true; fired = false; since = now; }
    Result update(uint64_t now, bool down, bool cancel) {
        if (!active) return Result::None;
        if (cancel) { active = false; return Result::None; }
        const bool long_press = now - since >= delay;
        if (!down) {
            active = false;
            if (fired) return Result::None;
            return long_press ? Result::Long : Result::Short;
        }
        if (long_press && !fired) { fired = true; return Result::Long; }
        return Result::None;
    }
};
