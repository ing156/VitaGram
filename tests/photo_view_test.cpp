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
    view.begin();
    now = 1;
    auto sticks = [&](int lx, int ly, int ry, int frames = 1, unsigned long long step = 16667) {
        for (int i = 0; i < frames; ++i) {
            view.sticks(lx, ly, ry, now);
            now += step;
        }
    };
    assert(PhotoView::stickAxis(104) == 0 && PhotoView::stickAxis(152) == 0);
    assert(PhotoView::stickAxis(0) == -1 && PhotoView::stickAxis(255) == 1);
    sticks(128, 128, 128);
    sticks(104, 152, 104, 60);
    assert(view.zoom == 1 && view.pan_x == 0 && view.pan_y == 0);
    sticks(0, 255, 128, 60); // No panning at fitted size.
    assert(view.zoom == 1 && view.pan_x == 0 && view.pan_y == 0);
    sticks(128, 128, 0, 200);
    assert(view.zoom == 5); // Up zooms in, capped at 500%.
    sticks(255, 255, 128, 10);
    assert(view.pan_x < 0 && view.pan_y < 0);
    view.pan_x = view.pan_y = 0;
    sticks(0, 0, 128, 10);
    assert(view.pan_x > 0 && view.pan_y > 0);
    vita2d_texture image;
    view.pan_x = view.pan_y = 10000;
    view.draw(&image);
    assert(std::abs(view.pan_x - 880) < 0.01f && std::abs(view.pan_y - 1088) < 0.01f);
    view.pan_x = view.pan_y = -10000;
    view.draw(&image);
    assert(std::abs(view.pan_x + 880) < 0.01f && std::abs(view.pan_y + 1088) < 0.01f);
    const float before_pan = view.pan_x;
    view.fingers = 1;
    sticks(0, 0, 255, 10);
    assert(view.zoom == 5 && view.pan_x == before_pan); // Touch has priority.
    view.fingers = 0;
    sticks(128, 128, 255, 200);
    assert(view.zoom == 1 && view.pan_x == 0 && view.pan_y == 0);
    view.zoom = 2;
    sticks(128, 128, 0, 1, 5000000);
    const float before_stall = view.zoom;
    sticks(128, 128, 0);
    assert(view.zoom - before_stall <= 0.126f); // Resume cannot jump to maximum.
    view.reset();
    sticks(128, 128, 0);
    assert(view.zoom == 1); // A newly opened photo starts at 100%.
    // Equal real time at 30 and 60 FPS gives the same zoom.
    PhotoView slow, fast;
    slow.sticks(128, 128, 0, 1); fast.sticks(128, 128, 0, 1);
    for (int i = 1; i <= 30; ++i) slow.sticks(128, 128, 0, 1 + i * 33334ULL);
    for (int i = 1; i <= 60; ++i) fast.sticks(128, 128, 0, 1 + i * 16667ULL);
    assert(std::abs(slow.zoom - fast.zoom) < 0.001f);
    // Pinch uses the same 500% limit.
    SceTouchData pinch{};
    pinch.reportNum = 2;
    pinch.report[0].id = 0; pinch.report[1].id = 1;
    pinch.report[1].x = 100;
    view.touch(pinch);
    pinch.report[1].x = 1000;
    view.touch(pinch);
    assert(view.zoom == 5);
    pinch.report[1].x = 20;
    view.touch(pinch);
    assert(view.zoom == 1);
    std::puts("photo view: rotation, stick zoom/pan, limits, dead zone, timing and touch priority: PASS");
}
