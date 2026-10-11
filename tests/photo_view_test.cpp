#include "photo_view.h"
#include <cassert>
#include <cstdio>

static SceMotionState motion;
static int starts = 0, stops = 0, reads = 0;
static bool failed = false;
int sceMotionStartSampling() { ++starts; return failed ? -1 : 0; }
int sceMotionStopSampling() { ++stops; return 0; }
int sceMotionGetState(SceMotionState* out) { ++reads; *out = motion; return 0; }

int main() {
    PhotoView view;
    unsigned long long now = 1;
    auto orient = [&](float x, float y, bool enabled = true, bool touching = false) {
        motion.basicOrientation.x = x; motion.basicOrientation.y = y;
        view.updateMotion(enabled, now, touching);
        now += 350000;
        view.updateMotion(enabled, now, touching);
        ++now;
    };
    orient(0, 1); assert(view.rotation == 0 && starts == 1);
    orient(1, 0); assert(view.rotation == 1);
    orient(0, -1); assert(view.rotation == 0); // Inverted console cannot flip the photo.
    orient(-1, 0); assert(view.rotation == 3);
    orient(0, 0); assert(view.rotation == 3); // Face up: retain orientation.
    orient(1, 0, true, true); assert(view.rotation == 3); // Touch owns the image.
    view.rotate(-1); assert(view.rotation == 2); // Manual 180 degrees is allowed.
    orient(0, 1); assert(view.rotation == 2 && !view.sampling && stops == 1);
    view.reset(); assert(view.rotation == 2); // Triangle resets zoom, not rotation.
    view.rotate(1); assert(view.rotation == 3);
    view.rotate(1); assert(view.rotation == 0);
    view.rotate(-1); assert(view.rotation == 3); // Wrap in both directions.
    view.begin(); assert(view.rotation == 0 && !view.manual_rotation);
    orient(1, 0, false); assert(view.rotation == 0 && starts == 1);
    view.rotate(1); view.rotate(1); assert(view.rotation == 2); // Works with gyro disabled.
    view.begin(); orient(-1, 0); assert(view.rotation == 3 && starts == 2);
    const int previous_reads = reads;
    view.updateMotion(false, now, false);
    assert(!view.sampling && stops == 2 && reads == previous_reads);
    failed = true;
    orient(1, 0); assert(!view.sampling && view.rotation == 3 && reads == previous_reads);
    failed = false;
    view.begin();
    motion.basicOrientation.x = 1;
    view.updateMotion(true, now, false);
    view.updateMotion(true, now + 349999, false); assert(view.rotation == 0);
    view.updateMotion(true, now + 350000, false); assert(view.rotation == 1);
    std::puts("photo view: gyro limits, debounce, manual rotation, disabled sampling and reopen: PASS");
}
