#pragma once
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <vita2d.h>
#include <algorithm>
#include <cmath>

// UI-thread only; zoom never allocates a second image or resamples on the CPU.
struct PhotoView {
    static constexpr float kMinZoom = 1.0f, kMaxZoom = 5.0f;
    float zoom = 1, pan_x = 0, pan_y = 0;
    float last_x = 0, last_y = 0, distance = 0;
    int fingers = 0, first_id = -1, second_id = -1;
    int rotation = 0, candidate = 0;
    unsigned long long candidate_since = 0;
    unsigned long long sticks_since = 0;
    bool captured = false, sampling = false, manual_rotation = false;

    void reset() { zoom = 1; pan_x = pan_y = 0; fingers = 0; distance = 0; captured = false; sticks_since = 0; }
    static float stickAxis(int value) {
        // Vita sticks report 0..255, with neutral near 128.
        const int offset = value - 128;
        const int magnitude = std::abs(offset);
        if (magnitude <= 24) return 0;
        const float amount = float(magnitude - 24) / (offset < 0 ? 104.0f : 103.0f);
        return offset < 0 ? -amount : amount;
    }
    void sticks(int lx, int ly, int ry, unsigned long long now) {
        // Cap elapsed time after stalls/resume; speed otherwise follows real time.
        const float dt = sticks_since && now > sticks_since ?
            std::min(0.05f, float(now - sticks_since) / 1000000.0f) : 0.0f;
        sticks_since = now;
        if (fingers) return; // Touch gestures own the image until release.
        const float next = std::clamp(zoom - stickAxis(ry) * 2.5f * dt, kMinZoom, kMaxZoom);
        const float ratio = next / zoom;
        pan_x *= ratio;
        pan_y *= ratio;
        zoom = next;
        if (zoom <= kMinZoom) { pan_x = pan_y = 0; return; }
        // Move the viewport in the stick direction, in screen coordinates even when rotated.
        pan_x -= stickAxis(lx) * 600.0f * dt;
        pan_y -= stickAxis(ly) * 600.0f * dt;
    }
    void begin() { rotation = candidate = 0; candidate_since = 0; manual_rotation = false; reset(); }
    void rotate(int direction) {
        rotation = (rotation + direction + 4) % 4;
        manual_rotation = true;
        candidate_since = 0;
        reset();
    }
    void updateMotion(bool open, unsigned long long now, bool touching) {
        if (!open || manual_rotation) {
            if (sampling) sceMotionStopSampling();
            sampling = false;
            candidate_since = 0;
            return;
        }
        if (!sampling) {
            sampling = sceMotionStartSampling() >= 0;
            if (!sampling) return;
        }
        SceMotionState state{};
        if (sceMotionGetState(&state) < 0 || touching) return;
        // Positive X means the right-hand end is up; compensate clockwise.
        const auto& g = state.basicOrientation;
        int next = rotation;
        if (g.x > 0.5f) next = 1;
        else if (g.x < -0.5f) next = 3;
        else if (g.y > 0.5f) next = 0;
        else if (g.y < -0.5f) next = 0; // Upside down: never auto-rotate by 180 degrees.
        // Lying flat: retain the last unambiguous orientation.
        if (next != candidate || !candidate_since) { candidate = next; candidate_since = now; }
        if (next != rotation && now - candidate_since >= 350000) {
            rotation = next; reset();
        }
    }
    // Consume the entire pinch/pan sequence, including the final release.
    bool touch(const SceTouchData& data) {
        int n = std::min(2, (int)data.reportNum);
        if (!n) { bool used = captured; captured = false; fingers = 0; return used; }
        float x = data.report[0].x * 0.5f, y = data.report[0].y * 0.5f, d = 0;
        int id0 = data.report[0].id, id1 = n == 2 ? data.report[1].id : -1;
        if (n == 2) {
            float x2 = data.report[1].x * 0.5f, y2 = data.report[1].y * 0.5f;
            d = std::hypot(x2-x, y2-y); x = (x+x2)*0.5f; y = (y+y2)*0.5f;
            captured = true;
        }
        if (zoom > 1.01f) captured = true;
        if (n == fingers && id0 == first_id && id1 == second_id) {
            if (n == 2 && distance > 8 && d > 8) {
                float next = std::clamp(zoom*d/distance, kMinZoom, kMaxZoom);
                float ratio = next/zoom;
                pan_x = x-480-(last_x-480-pan_x)*ratio;
                pan_y = y-272-(last_y-272-pan_y)*ratio;
                zoom = next;
            } else if (zoom > 1.01f) { pan_x += x-last_x; pan_y += y-last_y; }
        }
        last_x = x; last_y = y; distance = d; fingers = n; first_id = id0; second_id = id1;
        return captured;
    }
    void draw(vita2d_texture* image) {
        float w = vita2d_texture_get_width(image), h = vita2d_texture_get_height(image);
        if (w <= 0 || h <= 0) return;
        float bw = (rotation & 1) ? h : w, bh = (rotation & 1) ? w : h;
        // Fullscreen like the video player: the picture is fitted to the whole 960x544 screen.
        float scale = std::min(960.0f/bw, 544.0f/bh)*zoom;
        float mx = std::max(0.0f, (bw*scale-960)*0.5f);
        float my = std::max(0.0f, (bh*scale-544)*0.5f);
        pan_x = std::max(-mx, std::min(mx, pan_x));
        pan_y = std::max(-my, std::min(my, pan_y));
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(0, 0, 960, 544);
        vita2d_draw_texture_scale_rotate_hotspot(image, 480+pan_x, 272+pan_y,
            scale, scale, rotation*1.57079632679f, w*0.5f, h*0.5f);
        vita2d_disable_clipping();
    }
};
