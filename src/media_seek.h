#pragma once
#include <algorithm>
#include <cstdint>

namespace media_seek {
struct Source {
    int64_t chat = 0, message = 0;
    bool operator==(const Source& other) const { return chat == other.chat && message == other.message; }
    bool operator!=(const Source& other) const { return !(*this == other); }
};

// Select a target while holding Left/Right; commit once on release so the
// hardware decoder is not repeatedly reopened before it can produce a picture.
class HoldSeek {
public:
    enum class Action { None, Begin, Changed, Commit, Cancel };
    struct Event { Action action = Action::None; Source source; int64_t target = 0; };
    Event update(Source source, bool enabled, int direction, bool fresh, bool held,
                 int64_t position, int64_t duration, int64_t now) {
        if (active_ && (!enabled || source != source_ || now < sampled_ || now - sampled_ >= 1000000)) {
            active_ = false;
            sampled_ = now;
            return {Action::Cancel, source_, target_};
        }
        sampled_ = now;
        if (!enabled) return {};
        const int64_t maximum = std::max<int64_t>(0, duration - 500000);
        auto step = [&](int64_t from) {
            return std::clamp(std::clamp(from, int64_t(0), maximum) + direction * int64_t(10000000), int64_t(0), maximum);
        };
        if (!active_) {
            if (!direction || !fresh || !held) return {};
            source_ = source; direction_ = direction;
            target_ = step(position);
            if (target_ == std::clamp(position, int64_t(0), maximum)) return {};
            active_ = true; started_ = last_ = now;
            return {Action::Begin, source_, target_};
        }
        if (!held) {
            active_ = false;
            return {Action::Commit, source_, target_};
        }
        if (!direction) return {}; // Opposite arrows together never move the target.
        if (direction != direction_) {
            direction_ = direction; started_ = last_ = now;
        } else {
            const int64_t age = now - started_;
            const int64_t interval = age < 2000000 ? 400000 : age < 4000000 ? 250000 : 150000;
            if (now - last_ < interval) return {};
            last_ = now; // Slow frames generate one step, never a catch-up burst.
        }
        const int64_t next = step(target_);
        if (next == target_) return {};
        target_ = next;
        return {Action::Changed, source_, target_};
    }
    bool active() const { return active_; }
    int64_t targetUs() const { return target_; }
private:
    bool active_ = false;
    Source source_;
    int direction_ = 0;
    int64_t target_ = 0, sampled_ = 0, started_ = 0, last_ = 0;
};

inline float track_fraction(float x, float left, float width) {
    return std::clamp((x - left) / std::max(1.f, width), 0.f, 1.f);
}

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
