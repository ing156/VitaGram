#pragma once
#include <cstdint>

// Select-first must not arm exit, including during a system screenshot.
// Deliberate exit: hold Start, then press Select, hold both for two seconds.
class ExitGesture {
public:
    bool update(bool start, bool select, bool select_pressed, bool blocked, int64_t now) {
        if (blocked || !start || !select) { armed_ = false; return false; }
        if (select_pressed) { armed_ = true; since_ = now; }
        if (armed_ && now-since_ >= 2000000) { armed_ = false; return true; }
        return false;
    }
private:
    bool armed_ = false;
    int64_t since_ = 0;
};
