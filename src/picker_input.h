#pragma once
#include "reaction_input.h"

class PickerPress {
public:
    struct Target {
        int64_t chat = 0;
        int topic = 0, pack = 0, index = -1, file = 0;
        bool operator==(const Target& other) const {
            return chat == other.chat && topic == other.topic && pack == other.pack &&
                index == other.index && file == other.file;
        }
    };
    struct Result { bool send, consume_cross, consume_touch, preview; };
    Result update(bool context, Target target, bool cross_down, bool cross_fresh,
                  bool touching, bool touch_fresh, int touched_index, bool cancel, int64_t now) {
        const bool changed = !(target == target_);
        target_ = target;
        const auto cross = cross_.update(context, cross_down, cross_fresh,
            cancel || touching || (changed && !cross_fresh), target.chat, target.index,
            target.topic, now);
        const auto touch = touch_.update(context, touching, touch_fresh && touched_index >= 0,
            cancel || cross_down || touched_index != target.index || (changed && !touch_fresh),
            target.chat, target.index, target.topic, now);
        if (!context || cancel || changed || (cross_down && touching) || (!cross_down && !touching) ||
            (touching && touched_index != target.index)) preview_ = false;
        if ((cross.long_press && cross_down) || (touch.long_press && touching)) preview_ = true;
        return {cross.short_press || touch.short_press, cross.consume, touch.consume, preview_};
    }
private:
    Target target_;
    ReactionPress cross_, touch_;
    bool preview_ = false;
};
