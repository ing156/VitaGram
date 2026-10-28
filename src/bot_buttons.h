#pragma once
#include <jansson.h>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace bot_buttons {
inline std::string text(json_t* object, const char* key) {
    const char* value = json_string_value(json_object_get(object, key));
    return value ? value : "";
}
struct Button {
    std::string label, type, value;
    bool operator==(const Button& other) const {
        return label == other.label && type == other.type && value == other.value;
    }
};
struct Markup {
    bool inline_keyboard = false, one_time = false, force_reply = false;
    std::vector<std::vector<Button>> rows;
    bool empty() const { return rows.empty(); }
    bool operator==(const Markup& other) const {
        return inline_keyboard == other.inline_keyboard && one_time == other.one_time &&
            force_reply == other.force_reply && rows == other.rows;
    }
};
inline Markup parse(json_t* markup) {
    Markup result;
    const auto kind = text(markup, "@type");
    result.inline_keyboard = kind == "replyMarkupInlineKeyboard";
    result.force_reply = kind == "replyMarkupForceReply" || json_is_true(json_object_get(markup, "force_reply"));
    if (!result.inline_keyboard && kind != "replyMarkupShowKeyboard") return result;
    result.one_time = json_is_true(json_object_get(markup, "one_time"));
    size_t i, j; json_t *row, *item;
    json_array_foreach(json_object_get(markup, "rows"), i, row) {
        std::vector<Button> buttons;
        json_array_foreach(row, j, item) {
            auto* type = json_object_get(item, "type");
            Button button{text(item, "text"), text(type, "@type"), ""};
            if (button.label.empty()) continue;
            if (button.type == "inlineKeyboardButtonTypeCallback") button.value = text(type, "data");
            else if (button.type == "inlineKeyboardButtonTypeCopyText") button.value = text(type, "text");
            else button.value = text(type, "url");
            buttons.push_back(std::move(button));
        }
        if (!buttons.empty()) result.rows.push_back(std::move(buttons));
    }
    return result;
}
struct Cell {
    int row, column, x, y, w, h;
    bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
struct PanelGeometry {
    int left = 0;
    int x(int base) const { return left + base * (960 - left) / 960; }
    int width(int base) const { return base * (960 - left) / 960; }
    int span(int start, int width) const { return x(start + width) - x(start); }
    Cell cell(Cell base) const {
        const int right = x(base.x + base.w);
        base.x = x(base.x); base.w = right - base.x;
        return base;
    }
};
inline constexpr Cell menu_control{1, 0, 80, 498, 210, 44};
inline constexpr Cell keyboard_control{2, 0, 305, 498, 285, 44};
inline bool menu_app(const Markup& markup) {
    return markup.rows.size() == 1 && markup.rows[0].size() == 1 &&
        markup.rows[0][0].type == "inlineKeyboardButtonTypeWebApp";
}
inline bool toolbar_hit(bool menu, bool keyboard, int x, int y, int left = 0) {
    const PanelGeometry geometry {left};
    return (menu && geometry.cell(menu_control).hit(x, y)) || (keyboard && geometry.cell(keyboard_control).hit(x, y));
}
inline constexpr int64_t sheet_duration = 180000;
struct CommandSheet {
    int top, offset, first, count, left = 0;
    std::vector<Cell> cells() const {
        std::vector<Cell> result;
        for (int i = 0; i < count; ++i)
            result.push_back(PanelGeometry{left}.cell({first + i, 0, 32, top + offset + 44 + i * 64, 896, 56}));
        return result;
    }
    Cell back() const { return PanelGeometry{left}.cell({0, 0, 740, 502 + offset, 172, 40}); }
};
inline CommandSheet command_sheet(int total, int focus, int64_t elapsed, bool closing, int left = 0) {
    total = std::max(0, total);
    const int rows = std::max(1, std::min(4, total));
    const int height = 44 + rows * 64 - 8 + 72;
    const int first = std::clamp(focus, 0, std::max(0, total - 1)) / 4 * 4;
    const double t = std::clamp(double(elapsed) / sheet_duration, 0.0, 1.0);
    const double hidden = closing ? t * t * t : (1 - t) * (1 - t) * (1 - t);
    return {544 - height, int(height * hidden + 0.5), first, std::min(4, total - first), left};
}
// Wide Telegram rows become pairs so labels stay readable on the Vita screen.
inline std::vector<Cell> cells(const Markup& markup, int top, int height, int first = 0, int count = 6, int left = 48, int width = 864) {
    std::vector<Cell> result;
    int index = 0, visual_row = 0;
    for (int r = 0; r < (int)markup.rows.size(); ++r) {
        const auto& row = markup.rows[r];
        for (int c = 0; c < (int)row.size(); c += 2) {
            const int columns = std::min(2, (int)row.size() - c);
            bool visible_row = false;
            for (int n = 0; n < columns; ++n, ++index) {
                if (index < first || index >= first + count) continue;
                const int cell_width = (width - (columns - 1) * 12) / columns;
                result.push_back({r, c + n, left + n * (cell_width + 12), top + visual_row * (height + 10), cell_width, height});
                visible_row = true;
            }
            if (visible_row) ++visual_row;
        }
    }
    return result;
}
struct Page { int first = 0, count = 0, number = 0, total = 0; };
inline Page page(const Markup& markup, int focus, int rows_per_page) {
    Page result;
    int visual_row = 0, first = 0, count = 0;
    auto finish = [&] {
        if (!count) return;
        if (focus >= first && focus < first + count) { result.first = first; result.count = count; result.number = result.total; }
        ++result.total; first += count; count = 0;
    };
    for (const auto& row : markup.rows) for (int column = 0; column < (int)row.size(); column += 2) {
        if (visual_row && visual_row % std::max(1, rows_per_page) == 0) finish();
        const int amount = std::min(2, (int)row.size() - column);
        count += amount; ++visual_row;
    }
    finish();
    return result;
}
inline int preview_height(const Markup& markup) {
    if (!markup.inline_keyboard || markup.empty()) return 0;
    const auto preview = cells(markup, 0, 36, 0, 4);
    int total = 0;
    for (const auto& row : markup.rows) total += (int)row.size();
    return preview.back().y + preview.back().h + 8 + (total > 4 ? 40 : 0);
}
inline int size(const Markup& markup) {
    int count = 0;
    for (const auto& row : markup.rows) count += (int)row.size();
    return count;
}
} // namespace bot_buttons
