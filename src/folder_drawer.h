#pragma once
#include "ui_layout.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace chat_view {
// Coordinates here are physical screen pixels, before translating the chat surface.
struct FolderDrawer {
    static constexpr int rail = ui_layout::folder_rail, panel = 124, expanded = rail + panel;
    static constexpr int top = ui_layout::header_height;
    static int contentShift(int y, int shift) { return y < top ? 0 : shift; }
    static constexpr int row_top = 64, row_height = 52, rows = 8;
    static constexpr int64_t duration = 180000;
    bool open = false, down = false, owned = false, moved = false;
    int focus = 0, first = 0, start_x = 0, start_y = 0, step_y = 0;
    float origin = rail;
    int64_t since = 0;
    float offset(int64_t now) const {
        const float t = std::clamp((now-since)/(float)duration, 0.f, 1.f);
        const float ease = 1.f-(1.f-t)*(1.f-t)*(1.f-t);
        return origin + ((open ? expanded : rail)-origin)*ease;
    }
    float shift(int64_t now) const { return offset(now)-rail; }
    void show(bool value, int64_t now) { origin = offset(now); since = now; open = value; }
    void reset() { *this = FolderDrawer{}; }
    void reveal(int count) {
        focus = std::clamp(focus, 0, std::max(0, count-1));
        first = std::clamp(first, std::max(0, focus-rows+1), focus);
        first = std::min(first, std::max(0, count-rows));
    }
    int hit(int x, int y, int count) const {
        const int dy = y-row_top;
        const int index = first + dy/row_height;
        return x >= 6 && x < panel-6 && dy >= 0 && dy < rows*row_height &&
            dy%row_height < row_height-4 && index < count ? index : -1;
    }
    // Returns true for the entire owned gesture, including release; it never leaks a tap.
    bool touch(bool touching, int x, int y, int count, int64_t now, int& chosen) {
        chosen = -1;
        if (touching && !down) {
            owned = y >= top && y < ui_layout::height &&
                (open || (x >= 0 && x < std::max(24, (int)std::lround(offset(now)))));
            start_x = x; start_y = step_y = y; moved = false;
        }
        const bool consume = owned && (touching || down);
        if (consume && touching) {
            const int dx = x-start_x, dy = y-start_y;
            if (std::abs(dx)>14 || std::abs(dy)>14) moved = true;
            if (std::abs(dx)>30 && std::abs(dx)>std::abs(dy)) {
                const bool target = dx>0;
                if (open != target) show(target, now);
            } else if (open && start_x < panel && std::abs(dy)>std::abs(dx) && std::abs(y-step_y)>=row_height/2) {
                first = std::clamp(first + (y<step_y ? 1 : -1), 0, std::max(0,count-rows));
                if (count > 0) focus = std::clamp(focus, first, std::min(count-1, first+rows-1));
                step_y = y;
            }
        } else if (consume && !touching && !moved) {
            if (!open) show(true, now);
            else if (start_x >= panel || start_y < row_top) show(false, now);
            else { chosen = hit(start_x,start_y,count); if (chosen>=0) { focus=chosen; show(false,now); } }
        }
        down = touching;
        if (!touching) owned = false;
        return consume;
    }
};
}
