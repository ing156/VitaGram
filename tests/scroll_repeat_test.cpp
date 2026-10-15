#include "scroll_repeat.h"
#include <cassert>
#include <cstdio>
#include <vector>

static int sample(ScrollRepeat& repeat, int64_t time, int direction = 1,
                  bool fresh = false, int context = 1, double page = 8, int speed = 0) {
    repeat.beginFrame();
    return repeat.update(direction, fresh, context, page, time, speed);
}

int main() {
    ScrollInput input;
    auto check = [&](bool up, bool down, int y, int direction, bool fresh) {
        const auto state = input.sample(up, down, y);
        assert(state.direction == direction);
        assert(state.fresh == fresh);
    };
    check(false, false, 128, 0, false);
    check(false, false, 81, 0, false);
    check(false, false, 175, 0, false);
    check(false, false, 80, -1, true);
    check(false, false, 90, -1, false); // Hysteresis prevents jitter near engagement.
    check(false, false, 96, -1, false);
    check(false, false, 97, 0, false);
    check(false, false, 90, 0, false);
    check(false, false, 176, 1, true);
    check(false, false, 160, 1, false);
    check(false, false, 159, 0, false);
    check(false, false, 255, 1, true);
    check(true, false, 255, -1, true); // D-pad wins over the stick.
    check(true, false, 255, -1, false);
    check(true, true, 255, 0, false); // Opposing arrows stop both input sources.
    check(false, false, 255, 1, true);
    check(false, true, 255, 1, true); // Changing source restarts the hold.
    check(false, true, 255, 1, false);
    check(false, false, 255, 1, true);
    check(false, false, 0, -1, true); // Direct reversal needs no neutral frame.
    check(false, false, 128, 0, false);

    // Identical holds from either source must produce identical acceleration.
    ScrollInput stick_input, button_input;
    ScrollRepeat stick_repeat, button_repeat;
    int stick_steps = 0;
    for (int64_t now = 0; now <= 5000000; now += 10000) {
        const auto stick = stick_input.sample(false, false, 255);
        const auto button = button_input.sample(false, true, 128);
        const int stick_event = sample(stick_repeat, now, stick.direction, stick.fresh);
        const int button_event = sample(button_repeat, now, button.direction, button.fresh);
        assert(stick_event == button_event);
        stick_steps += stick_event;
    }
    assert(stick_steps > 20);
    const auto neutral = stick_input.sample(false, false, 128);
    assert(sample(stick_repeat, 5010000, neutral.direction, neutral.fresh) == 0);

    ScrollRepeat repeat;
    assert(sample(repeat, 0, 1, true) == 0);
    assert(sample(repeat, 349999) == 0);
    assert(sample(repeat, 350000) == 1);
    assert(sample(repeat, 360000, 0) == 0);
    assert(sample(repeat, 800000, 0) == 0);
    assert(sample(repeat, 810000, -1, true) == 0);
    assert(sample(repeat, 1159999, -1) == 0);
    assert(sample(repeat, 1160000, -1) == -1);

    // A different menu, skipped input owner, or resumed app cannot inherit a hold.
    assert(sample(repeat, 1170000, -1, false, 2) == 0);
    assert(sample(repeat, 1770000, -1, false, 2) == 0);
    repeat.beginFrame();
    assert(sample(repeat, 1780000, -1, false, 2) == 0);
    assert(sample(repeat, 1790000, -1, true, 2) == 0);
    assert(sample(repeat, 2140000, -1, false, 2) == -1);
    assert(sample(repeat, 4140000, -1, false, 2) == 0);
    assert(sample(repeat, 4500000, -1, false, 2) == 0);

    // Exercise different page geometries and frame rates against the speed limit.
    for (int speed = 0; speed < ScrollRepeat::kSpeedChoices; ++speed) {
      assert(ScrollRepeat::pagesPerSecond(speed) == 2.0 * (speed + 1));
      for (double page : {0.5, 1.0, 3.0, 8.0, 15.0}) {
        for (int64_t frame : {8333, 16667, 33333, 100000}) {
            ScrollRepeat r;
            sample(r, 0, 1, true, 1, page, speed);
            std::vector<int64_t> events{0};
            for (int64_t now = frame; now <= 8000000; now += frame) {
                if (sample(r, now, 1, false, 1, page, speed)) {
                    assert(now - events.back() >= 1000000.0 / (page * ScrollRepeat::pagesPerSecond(speed)));
                    events.push_back(now);
                }
            }
            assert(events.size() > 3);
            assert(events[1] >= 350000);
            assert(events.back() - events[events.size() - 2] < events[1]);
        }
      }
    }

    // At full acceleration each preset traverses exactly its limit, within
    // one navigation step of sampling/rounding. A speed decrease applies now.
    for (int speed = 0; speed < ScrollRepeat::kSpeedChoices; ++speed) {
        ScrollRepeat r;
        sample(r, 0, 1, true, 1, 3, speed);
        int steps = 0;
        for (int64_t now = 1000; now <= 6000000; now += 1000) {
            const int event = sample(r, now, 1, false, 1, 3, speed);
            if (now > 5000000) steps += event;
        }
        assert(std::abs(steps - 3 * ScrollRepeat::pagesPerSecond(speed)) <= 1);
        assert(sample(r, 6000001, 1, false, 1, 3, 0) == 0);
    }

    // A stalled frame emits one event and leaves no backlog for the next frame.
    ScrollRepeat stalled;
    sample(stalled, 0, 1, true);
    for (int64_t now = 10000; now <= 3000000; now += 10000) sample(stalled, now);
    assert(sample(stalled, 3500000) == 1);
    assert(sample(stalled, 3500001) == 0);
    // Shrinking the viewport must immediately enforce its slower limit.
    assert(sample(stalled, 3562500, 1, false, 1, 1) == 0);
    std::puts("Scroll repeat tests passed");
}
