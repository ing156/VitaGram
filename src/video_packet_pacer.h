#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

// Keep compressed input near playback time; let it advance in real time if audio is stalled.
class VideoPacketPacer {
public:
    bool ready(int64_t packet_us, int64_t clock_us, int64_t now_us, bool stalled, bool paused) {
        if (paused) { stall_started_ = 0; return false; }
        if (clock_us < 0) { stall_started_ = 0; reference_ = -1; return true; }
        // Brief audio recovery must not revoke input already allowed during a stall.
        int64_t reference = std::max(clock_us, reference_);
        if (stalled) {
            if (!stall_started_) { stall_started_ = now_us; stall_clock_ = reference; }
            const int64_t elapsed = std::max<int64_t>(0, now_us - stall_started_);
            reference = std::max(reference, stall_clock_ > INT64_MAX - elapsed ? INT64_MAX : stall_clock_ + elapsed);
        } else stall_started_ = 0;
        reference_ = reference;
        return packet_us <= reference || (packet_us - reference <= 4000000);
    }
private:
    int64_t stall_started_ = 0, stall_clock_ = 0, reference_ = -1;
};
