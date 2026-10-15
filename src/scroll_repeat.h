#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Sample every frame, including outside lists, so entering a menu with an
// already deflected stick does not count as a new press. D-pad takes priority.
class ScrollInput {
public:
    struct State { int direction; bool fresh; };

    State sample(bool up, bool down, int left_y) {
        const int offset = left_y - 128;
        if (offset <= -48) stick_ = -1;
        else if (offset >= 48) stick_ = 1;
        else if ((stick_ < 0 && offset > -32) || (stick_ > 0 && offset < 32)) stick_ = 0;
        const int source = up || down ? 1 : stick_ ? 2 : 0;
        const int direction = source == 1 ? (up == down ? 0 : up ? -1 : 1) : stick_;
        const bool fresh = direction && (direction != direction_ || source != source_);
        direction_ = direction;
        source_ = source;
        return {direction, fresh};
    }

private:
    int stick_ = 0, direction_ = 0, source_ = 0;
};

// One event per sample: slow frames never accumulate a burst of navigation.
class ScrollRepeat {
public:
    static constexpr int kSpeedChoices = 3;
    static double pagesPerSecond(int speed) {
        return 2.0 * (1 + std::clamp(speed, 0, kSpeedChoices - 1));
    }
    void beginFrame() { ++frame_; }

    int update(int direction, bool fresh, int context, double page_steps, int64_t now, int speed = 0) {
        const bool continuous = frame_ == last_frame_ + 1 && context == context_ &&
            direction == direction_ && now >= sampled_at_ && now - sampled_at_ < 1000000;
        last_frame_ = frame_;
        context_ = context;
        sampled_at_ = now;
        if (!direction) { direction_ = 0; armed_ = false; return 0; }
        if (!continuous || fresh) {
            direction_ = direction;
            armed_ = fresh;
            started_at_ = last_at_ = now;
            first_ = true;
            return 0; // The caller already handles the physical press.
        }
        if (!armed_) return 0; // A held button must not carry into another menu.
        // Rate is measured in vertical navigation steps per second. Smoothstep
        // reaches the selected ceiling after a two-second ramp. All modes
        // start at the same rate, so short holds remain easy to control.
        const double maximum_rate = std::max(0.01, page_steps * pagesPerSecond(speed));
        const double initial_rate = std::min(3.0, std::max(0.005, page_steps));
        const double t = std::clamp((now - started_at_ - 350000) / 2000000.0, 0.0, 1.0);
        const double curve = t * t * (3.0 - 2.0 * t);
        const double rate = initial_rate + (maximum_rate - initial_rate) * curve;
        const int64_t interval = (int64_t)std::ceil(1000000.0 / rate);
        if (now - last_at_ < std::max<int64_t>(first_ ? 350000 : 0, interval)) return 0;
        last_at_ = now;
        first_ = false;
        return direction;
    }

private:
    uint64_t frame_ = 0, last_frame_ = 0;
    int context_ = 0, direction_ = 0;
    bool armed_ = false, first_ = true;
    int64_t sampled_at_ = 0, started_at_ = 0, last_at_ = 0;
};
