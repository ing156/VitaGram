#pragma once
#include "ui_layout.h"
#include <algorithm>

namespace chat_view {
inline constexpr int sidebar = 224;
struct ToolWindow {
    static constexpr int left = 16, width = 928;
    static constexpr int back_left = 816, back_top = 38, back_width = 128, back_height = 26;
    static bool backHit(int x, int y) {
        return x >= back_left && x < back_left+back_width && y >= back_top && y < back_top+back_height;
    }
};
enum class Pane { None, Chats, History };
struct Layout {
    bool combined = false;
    int top = ui_layout::history_top;
    bool folders = false;
    int folderTop() const { return ui_layout::content_top + (combined ? 0 : ui_layout::search_shift); }
    int searchTop() const { return ui_layout::content_top; }
    int listTop() const { return folders && !combined ? ui_layout::chat_top_folders : ui_layout::chat_top_plain; }
    int left() const { return combined ? sidebar : 0; }
    int x(int base) const { return left() + base; }
    int y(int base) const { return base + top - ui_layout::history_top; }
    int width(int base) const { return base - left(); }
    int attachLeft() const { return x(ui_layout::attach_left); }
    int attachTop(int count) const { return ui_layout::menu_bottom - count*ui_layout::menu_row - ui_layout::menu_pad; }
    int attachHit(int px, int py, int count) const {
        const int row = py-attachTop(count);
        return px >= attachLeft() && px < attachLeft()+ui_layout::attach_width &&
            row >= 0 && row < count*ui_layout::menu_row && row%ui_layout::menu_row >= 2 &&
            row%ui_layout::menu_row < 2+ui_layout::menu_focus_height ? row/ui_layout::menu_row : -1;
    }
    int menuRows() const { return !combined ? ui_layout::menu_max_rows : std::min(ui_layout::menu_max_rows, (ui_layout::menu_bottom - top - 2*ui_layout::menu_pad - 4) / ui_layout::menu_row); }
    int topicTop() const { return combined ? top : 70; }
    int topicRows() const { return std::min(8, (ui_layout::height-topicTop()-54)/50); }
    int topicHit(int px, int py) const {
        const int row = py-topicTop();
        return px >= x(20) && px < 940 && row >= 0 && row < topicRows()*50 && row%50 < 48 ? row/50 : -1;
    }
    int pollHit(int px, int py) const {
        const int row = py-y(ui_layout::poll_top);
        return px >= x(ui_layout::poll_left) && px < ui_layout::poll_left+ui_layout::poll_width &&
            row >= 0 && row < 5*ui_layout::poll_step && row%ui_layout::poll_step < ui_layout::poll_height ? row/ui_layout::poll_step : -1;
    }
    int center() const { return (left() + ui_layout::width) / 2; }
    int bubbleLeft() const { return left() + ui_layout::bubble_left; }
    int bubbleWidth() const { return std::min(ui_layout::bubble_max_width, ui_layout::bubble_right - bubbleLeft()); }
    int bodyWidth() const { return bubbleWidth() - 32; }
    int listInset() const { return combined && folders ? ui_layout::folder_rail : 0; }
    int listLeft() const { return ui_layout::list_left + listInset(); }
    int listWidth() const { return combined ? sidebar - 20 - listInset() : ui_layout::list_width; }
    bool searchHit(int x, int y) const {
        return x >= listLeft() && x < listLeft() + listWidth() &&
            y >= searchTop() && y < searchTop() + ui_layout::search_height;
    }
    bool folderBandHit(int x, int y) const {
        return folders && !combined && x >= ui_layout::list_left && x < ui_layout::list_left + ui_layout::list_width &&
            y >= folderTop() && y < folderTop() + ui_layout::folder_height;
    }
    bool historyHit(int x, int y, int bottom) const {
        return x >= left() && x < ui_layout::width && y >= top && y < bottom;
    }
    bool listHit(int x, int y, int list_top, int row_height, int offset) const {
        return x >= listLeft() && x < listLeft() + listWidth() &&
            y >= list_top && y < ui_layout::height && y - list_top + offset >= 0 &&
            (y - list_top + offset) % row_height < row_height - 2;
    }
    Pane scrollPane(int x, int y, int bottom) const {
        if (!combined) return Pane::None;
        if (x >= listInset() && x < sidebar && y >= listTop() && y < ui_layout::height) return Pane::Chats;
        return historyHit(x, y, bottom) ? Pane::History : Pane::None;
    }
};
inline Layout layout(bool combined, bool folders) {
    Layout result{combined, ui_layout::history_top, folders};
    if (combined) result.top = result.searchTop();
    return result;
}
}
