#include "media_seek.h"
#include <cassert>
#include <cstdio>

using media_seek::HoldSeek;
using Action = HoldSeek::Action;
constexpr media_seek::Source source{42, 501};
constexpr int64_t second = 1000000, duration = 1200 * second;

static void hold_and_acceleration() {
    HoldSeek seek;
    auto update = [&](int64_t now, int direction = 1, bool fresh = false, bool held = true) {
        return seek.update(source, true, direction, fresh, held, 100 * second, duration, now);
    };
    assert(update(0, 1, true).action == Action::Begin && seek.targetUs() == 110 * second);
    assert(update(399999).action == Action::None);
    assert(update(400000).action == Action::Changed && seek.targetUs() == 120 * second);
    // Continuous sampling at device frame cadence: measure the actual rate in
    // three one-second windows, without feeding a fake burst of button presses.
    int slow = 0, medium = 0, fast = 0;
    int64_t previous = seek.targetUs();
    for (int64_t now = 410000; now <= 5500000; now += 10000) {
        const auto event = update(now);
        if (event.action == Action::Changed) {
            assert(event.target - previous == 10 * second);
            if (now >= 500000 && now < 1500000) ++slow;
            if (now >= 2500000 && now < 3500000) ++medium;
            if (now >= 4500000 && now < 5500000) ++fast;
            previous = event.target;
        }
    }
    assert(slow > 0 && medium > slow && fast > medium);
    const auto end = update(5510000, 0, false, false);
    assert(end.action == Action::Commit && end.source == source && end.target == previous && !seek.active());
    assert(update(5520000, 0, false, false).action == Action::None); // Exactly one seek on release.
}

static void interruption_and_conflicts() {
    HoldSeek seek;
    assert(seek.update(source, true, -1, true, true, 100 * second, duration, 0).target == 90 * second);
    auto event = seek.update(source, false, -1, false, true, 100 * second, duration, 10000);
    assert(event.action == Action::Cancel && !seek.active());
    assert(seek.update(source, true, -1, false, true, 100 * second, duration, 20000).action == Action::None);
    // Closing a menu while an arrow is already held cannot start a seek.
    assert(seek.update(source, true, -1, true, true, 100 * second, duration, 30000).action == Action::Begin);
    event = seek.update({42, 502}, true, -1, false, true, 200 * second, duration, 40000);
    assert(event.action == Action::Cancel && event.source == source); // Never apply an old target to a new message.
    assert(seek.update(source, true, 0, true, true, 100 * second, duration, 50000).action == Action::None);
    assert(seek.update(source, true, 1, true, true, 100 * second, duration, 60000).action == Action::Begin);
    assert(seek.update(source, true, 0, true, true, 100 * second, duration, 70000).action == Action::None);
    assert(seek.targetUs() == 110 * second); // Both arrows together freeze the target.
    assert(seek.update(source, true, -1, false, true, 100 * second, duration, 80000).target == 100 * second);
    assert(seek.update(source, true, -1, false, true, 100 * second, duration, 790000).target == 90 * second);
    assert(seek.update(source, true, -1, false, true, 100 * second, duration, 1790000).action == Action::Cancel);
    assert(!seek.active()); // App pause or sleep discards the held gesture, with no catch-up burst.
}

static void bounds_and_timeline() {
    HoldSeek seek;
    auto event = seek.update(source, true, -1, true, true, 5 * second, duration, 0);
    assert(event.action == Action::Begin && event.target == 0);
    assert(seek.update(source, true, 0, false, false, 5 * second, duration, 10000).action == Action::Commit);
    assert(seek.update(source, true, -1, true, true, 0, duration, 20000).action == Action::None);
    event = seek.update(source, true, 1, true, true, 1199 * second, duration, 30000);
    assert(event.target == duration - 500000);
    assert(seek.update(source, true, 1, false, true, 1199 * second, duration, 440000).action == Action::None);
    assert(seek.update(source, true, 0, false, false, 1199 * second, duration, 450000).action == Action::Commit);
    // The same recorded strip geometry maps both a tap and a drag, including
    // when the strip is transparent. Out-of-track drags clamp to its endpoints.
    assert(media_seek::track_fraction(112, 112, 620) == 0.f);
    assert(media_seek::track_fraction(422, 112, 620) == 0.5f);
    assert(media_seek::track_fraction(732, 112, 620) == 1.f);
    assert(media_seek::track_fraction(-100, 112, 620) == 0.f);
    assert(media_seek::track_fraction(960, 112, 620) == 1.f);
}

int main() {
    hold_and_acceleration(); interruption_and_conflicts(); bounds_and_timeline();
    std::puts("Fullscreen seek input: 10-second steps, acceleration, single commit, context cancellation, bounds and timeline mapping PASS");
}
