#pragma once
#include "ui_themes.h"
#include "ui_layout.h"

struct ThemePicker {
    static constexpr int left = ui_layout::theme_left, top = ui_layout::theme_top, width = ui_layout::theme_width,
        header = ui_layout::theme_header, row_height = ui_layout::theme_row;
    static constexpr int visible = ui_layout::theme_visible;
    int count = ui_themes::count();
    static constexpr int height = header + visible * row_height + 12;
    bool active = false;
    int focused = 0;
    int first = 0;
    void reveal() { first = std::max(0, std::min(first, focused)); first = std::max(first, focused - visible + 1); }
    void open(int theme) { focused = std::max(0, std::min(count - 1, theme)); first = 0; reveal(); active = true; }
    void move(int direction) { focused = (focused + count + direction) % count; reveal(); }
    void scroll(int rows) {
        first = std::max(0, std::min(count - visible, first + rows));
        focused = std::max(first, std::min(first + visible - 1, focused));
    }
    int hit(int x, int y) const {
        if (x < left || x >= left + width || y < top + header || y >= top + header + visible * row_height) return -1;
        const int index=first + (y - top - header) / row_height;
        return index<count ? index : -1;
    }
    void close() { active = false; }
};
