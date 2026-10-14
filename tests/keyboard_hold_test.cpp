#include "keyboard_hold.h"
#include <cassert>
#include <cstdio>

int main() {
    using R = KeyboardHold::Result;
    KeyboardHold hold;
    assert(hold.update(1, false, false) == R::None);
    hold.start(100);
    assert(hold.update(200, true, false) == R::None);
    assert(hold.update(300, false, false) == R::Short);
    assert(hold.update(400, false, false) == R::None);
    hold.start(1000);
    assert(hold.update(1000 + KeyboardHold::delay - 1, true, false) == R::None);
    assert(hold.update(1000 + KeyboardHold::delay, true, false) == R::Long);
    assert(hold.update(1000 + KeyboardHold::delay + 1, true, false) == R::None);
    assert(hold.update(1000 + KeyboardHold::delay + 2, false, false) == R::None);
    // A delayed frame that first sees release must still resolve a long press.
    hold.start(2000);
    assert(hold.update(2000 + KeyboardHold::delay, false, false) == R::Long);
    assert(hold.update(2000 + KeyboardHold::delay + 1, false, false) == R::None);
    // Moving away or switching layout cancels even after the threshold.
    hold.start(3000);
    assert(hold.update(3000 + KeyboardHold::delay, true, true) == R::None);
    assert(hold.update(3000 + KeyboardHold::delay + 1, false, false) == R::None);
    hold.start(4000);
    assert(hold.update(4001, false, true) == R::None);
    std::puts("keyboard hold: short/long, threshold, delayed release, cancellation and no duplicates: PASS");
}
