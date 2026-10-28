#include "custom_keyboard.h"
#include "keyboard_layout.h"
#include "keyboard_hold.h"
#include "keyboard_case.h"
#include "keyboard_languages.h"
#include "i18n.h"
#include "media_render.h"
#include "ui_skin.h"
#include "ui_color.h"
#include "performance.h"
#include "touch_history.h"
#include "diag_log.h"
#include <array>
#include <cmath>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <algorithm>
#include <vector>
#include <cstdlib>
#include <cstdio>

namespace {
struct Colors {
    unsigned background, panel, key, utility, active, ink, muted, accent, on_accent, separator, focus, header_ink;
    unsigned key_ink, utility_ink, active_ink, key_muted, utility_muted, active_muted;
};

Colors colors(bool dark) {
    dark = ui_skin::is_dark(dark);
    const std::array<unsigned,11> roles{{
        ui_skin::color("background", dark ? RGBA8(7,26,52,255) : RGBA8(237,245,253,255)),
        ui_skin::color("menu", dark ? RGBA8(8,28,54,255) : RGBA8(245,250,255,255)),
        ui_skin::color("surface", dark ? RGBA8(17,43,75,255) : RGBA8(255,255,255,255)),
        ui_skin::color("surface_secondary", dark ? RGBA8(23,55,89,255) : RGBA8(227,240,252,255)),
        ui_skin::color("selected", dark ? RGBA8(16,92,180,255) : RGBA8(201,231,255,255)),
        ui_skin::color("text", dark ? RGBA8(242,247,255,255) : RGBA8(16,42,80,255)),
        ui_skin::color("muted", dark ? RGBA8(201,217,237,255) : RGBA8(73,100,133,255)),
        ui_skin::color("accent", dark ? RGBA8(20,154,255,255) : RGBA8(8,126,234,255)),
        ui_skin::color("border", dark ? RGBA8(48,86,123,255) : RGBA8(193,218,241,255)),
        ui_skin::color("focus", dark ? RGBA8(54,186,255,255) : RGBA8(8,140,250,255)),
        ui_skin::color("header_text", dark ? RGBA8(242,247,255,255) : RGBA8(16,42,80,255))
    }};
    static std::array<unsigned,11> previous{};
    static Colors c{};
    if (roles != previous) {
        previous = roles;
        c.background=roles[0]; c.panel=roles[1]; c.key=roles[2]; c.utility=roles[3]; c.active=roles[4];
        c.ink=roles[5]; c.muted=roles[6]; c.accent=roles[7]; c.separator=roles[8]; c.focus=roles[9]; c.header_ink=roles[10];
        c.on_accent=ui_color::on(c.accent);
        c.key_ink=ui_color::tone(c.ink,c.key,c.key,4.5);
        c.utility_ink=ui_color::tone(c.ink,c.utility,c.utility,4.5);
        c.active_ink=ui_color::tone(c.ink,c.active,c.active,4.5);
        c.key_muted=ui_color::tone(c.muted,c.key,c.key,4.5);
        c.utility_muted=ui_color::tone(c.muted,c.utility,c.utility,4.5);
        c.active_muted=ui_color::tone(c.muted,c.active,c.active,4.5);
    }
    return c;
}

void triangle(float cx, float cy, float r, int direction, unsigned color) {
    // direction: 0 up, 1 down, 2 left, 3 right. Rows of 1 px keep it independent of vita2d primitives.
    const int n = (int)(2 * r);
    for (int i = 0; i <= n; ++i) {
        float half = i * 0.62f;
        switch (direction) {
            case 0: vita2d_draw_rectangle(cx - half, cy - r + i, 2 * half, 1, color); break;
            case 1: vita2d_draw_rectangle(cx - half, cy + r - i, 2 * half, 1, color); break;
            case 2: vita2d_draw_rectangle(cx - r + i, cy - half, 1, 2 * half, color); break;
            default: vita2d_draw_rectangle(cx + r - i, cy - half, 1, 2 * half, color); break;
        }
    }
}

// Dense cached corners and a one-pixel alpha fringe keep every key/popup contour smooth.
void rounded_key_base(float x, float y, float w, float h, float radius, unsigned color) {
    constexpr int steps=16, points=4*(steps+1), fan_count=points+2, fringe_count=2*(points+1);
    struct Normal { float x,y; };
    static const auto normals=[] {
        std::array<Normal,points> result{};
        for (int corner=0;corner<4;++corner) for (int step=0;step<=steps;++step) {
            const float angle=(-90+corner*90+step*90.f/steps)*3.14159265358979323846f/180;
            result[corner*(steps+1)+step]={std::cos(angle),std::sin(angle)};
        }
        return result;
    }();
    radius=std::max(0.f,std::min(radius,std::min(w,h)/2));
    auto* fan=static_cast<vita2d_color_vertex*>(vita2d_pool_memalign((fan_count+fringe_count)*sizeof(vita2d_color_vertex),4));
    if (!fan) return;
    auto* fringe=fan+fan_count;
    fan[0]={x+w/2,y+h/2,0.5f,color};
    for (int i=0;i<points;++i) {
        const int corner=i/(steps+1);
        const float cx=x+(corner<2?w-radius:radius),cy=y+(corner==0 || corner==3?radius:h-radius);
        fan[i+1]={cx+normals[i].x*radius,cy+normals[i].y*radius,0.5f,color};
    }
    fan[fan_count-1]=fan[1];
    for (int i=0;i<=points;++i) {
        const int index=i%points;
        fringe[2*i]=fan[index+1];
        fringe[2*i+1]={fringe[2*i].x+normals[index].x,fringe[2*i].y+normals[index].y,0.5f,color & 0x00FFFFFFu};
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN,fan,fan_count);
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP,fringe,fringe_count);
}

void draw_symbol(int symbol, float cx, float cy, float r, unsigned color, unsigned background) {
    switch (symbol) {
        case 0: case 1: case 2: case 3: triangle(cx, cy, r, symbol, color); break;
        case 4:
            for (float t = -r * 0.8f; t <= r * 0.8f; t += 1.0f) {
                vita2d_draw_fill_circle(cx + t, cy + t, 2.4f, color);
                vita2d_draw_fill_circle(cx + t, cy - t, 2.4f, color);
            }
            break;
        case 5:
            vita2d_draw_fill_circle(cx, cy, r, color);
            vita2d_draw_fill_circle(cx, cy, r - 3.6f, background);
            break;
        case 6:
            triangle(cx, cy + 1, r, 0, color);
            triangle(cx, cy + r * 0.32f, r * 0.55f, 0, background);
            break;
        default:
            vita2d_draw_rectangle(cx - r, cy - r, 2 * r, 2 * r, color);
            vita2d_draw_rectangle(cx - r + 3.6f, cy - r + 3.6f, 2 * r - 7.2f, 2 * r - 7.2f, background);
            break;
    }
}

// Small controller badges live inside keys; use the theme atlas when available.
void shortcut_badge(vita2d_pgf* font, int button, int right, int top, unsigned color, unsigned fill) {
    ui_skin::Sprite sprite;
    if (ui_skin::button(button, sprite) && sprite.vw > 0 && sprite.vh > 0) {
        const float width = button == 5 ? 48.0f : button >= 7 ? 22.0f : 16.0f;
        const float scale = std::min(16.0f / sprite.vh, width / sprite.vw);
        vita2d_draw_texture_tint_part_scale(sprite.texture,
            right - (sprite.vx + sprite.vw)*scale, top - sprite.vy*scale,
            sprite.x, sprite.y, sprite.w, sprite.h, scale, scale, ui_skin::kWhite);
    } else if (button <= 3) {
        const int symbols[] = {6, 7, 4, 5}; // Triangle, Square, Cross, Circle.
        draw_symbol(symbols[button], right - 7, top + 7, 5, color, fill);
    } else {
        const char* label = button == 5 ? "START" : button == 7 ? "L" : "R";
        const int width = rich_text_width(font, 0.3f, label);
        rich_text_draw(font, right - width, top + 10, color, 0.3f, label);
    }
}

enum class Act { Char, Shift, Symbols, Language, Space, Delete, Clear, Cancel, Newline, Ok };
struct Key { int x, y, w, h; Act act; std::string label; std::vector<std::string> alternatives{}; };
size_t count_chars(const std::string& text) {
    size_t n = 0;
    for (unsigned char b : text) if ((b & 0xc0) != 0x80) ++n;
    return n;
}

}

struct CustomKeyboard::Impl {
    bool split_layout = false;
    int split_level = 1; // Small, medium, wide.
    struct Gesture {
        bool captured = false, armed = false, finished = false, rejected = false;
        int single_id = -1, left_id = -1, right_id = -1;
        int left_x = 0, left_y = 0, right_x = 0, right_y = 0;
        uint64_t single_since = 0, since = 0;
    } gesture;
    // Reserve a chord until every finger lifts, including its final one-finger tail.
    bool split_gesture(const SceTouchData& touches, uint64_t now) {
        if (!touches.reportNum) {
            const bool captured = gesture.captured;
            gesture = Gesture{};
            return captured;
        }
        if (gesture.rejected) return false;
        if (!gesture.captured && touches.reportNum == 1) {
            if (gesture.single_id != touches.report[0].id) {
                gesture.single_id = touches.report[0].id; gesture.single_since = now;
            }
            return false;
        }
        if (!gesture.captured) {
            gesture.since = now;
            if (touches.reportNum != 2 || (gesture.single_id >= 0 &&
                (now < gesture.single_since || now - gesture.single_since > 180000))) return false;
            const auto& a = touches.report[touches.report[0].x < touches.report[1].x ? 0 : 1];
            const auto& b = touches.report[touches.report[0].x < touches.report[1].x ? 1 : 0];
            gesture.left_id=a.id; gesture.right_id=b.id;
            gesture.left_x=a.x/2; gesture.left_y=a.y/2; gesture.right_x=b.x/2; gesture.right_y=b.y/2;
            gesture.armed = a.id != b.id && gesture.left_y >= 250 && gesture.right_y >= 250 &&
                gesture.left_y < 544 && gesture.right_y < 544 && std::abs(gesture.left_y-gesture.right_y) <= 100 &&
                (split_layout ? gesture.left_x < 400 && gesture.right_x > 560 :
                 gesture.left_x >= 240 && gesture.left_x < 480 && gesture.right_x > 480 && gesture.right_x <= 720);
            gesture.captured = gesture.armed;
            return gesture.captured;
        }
        if (touches.reportNum != 2 || now < gesture.since || now-gesture.since > 1500000) gesture.armed = false;
        if (!gesture.armed) return true;
        const SceTouchReport *left=nullptr, *right=nullptr;
        for (unsigned i=0;i<touches.reportNum;++i) {
            if (touches.report[i].id == gesture.left_id) left=&touches.report[i];
            if (touches.report[i].id == gesture.right_id) right=&touches.report[i];
        }
        if (!left || !right || std::abs(left->y/2-gesture.left_y)>48 || std::abs(right->y/2-gesture.right_y)>48) {
            gesture.armed=false; return true;
        }
        const int direction=split_layout ? -1 : 1;
        if (direction*(gesture.left_x-left->x/2)>=56 && direction*(right->x/2-gesture.right_x)>=56) {
            split_layout=!split_layout; gesture.armed=false; gesture.finished=true;
        }
        return true;
    }
    KeyboardCase casing;
    bool symbols = false, cancel = false, auto_capitalize = true;
    std::string code = "en";
    bool language_chosen = false, allow_system = false;
    std::vector<std::string> preferences, enabled{"en"};
    std::vector<const keyboard_layout::Layout*> language_choices() const {
        std::vector<const keyboard_layout::Layout*> result;
        for (const auto& profile : keyboard_layout::all())
            if (keyboard_languages::contains(enabled, profile.code)) result.push_back(&profile);
        return result;
    }
    void apply_languages() {
        enabled = keyboard_languages::resolve(preferences, i18n::systemLanguage());
        enabled.erase(std::remove_if(enabled.begin(), enabled.end(), [&](const std::string& value) {
            return keyboard_layout::get(value).code != value || (!allow_system && keyboard_layout::get(value).system());
        }), enabled.end());
        if (enabled.empty()) enabled.push_back("en");
    }
    const keyboard_layout::Layout& layout() const { return keyboard_layout::get(code); }
    bool choose_language(const std::string& requested, bool keep_focus = false) {
        const auto& chosen = keyboard_layout::get(requested);
        if (chosen.code != requested || !keyboard_languages::contains(enabled, requested) || (chosen.system() && !allow_system)) return false;
        code = requested; language_chosen = true;
        symbols = false;
        casing.otherAction();
        hold = Hold{}; deletion = DeleteRepeat{}; feedback_down = false; feedback_until = 0;
        row = keep_focus ? 3 : 0; col = keep_focus ? 2 : 0;
        geometry();
        return true;
    }
    bool secret = false, message = false, was_touching = false;
    int row = 0, col = 0;
    bool touch_mode = false, feedback_down = false;
    Key feedback{}, pending_touch{};
    bool pending_touch_active = false;
    uint64_t pending_touch_since = 0;
    uint64_t clock = 0, feedback_until = 0;
    void show_press(const Key& key) {
        feedback = key;
        feedback_until = clock + 140000;
        feedback_down = true;
    }
    enum class DeleteSource { None, Touch, Cross, Square };
    struct DeleteRepeat {
        static constexpr uint64_t delay = 350000, interval = 60000;
        DeleteSource source = DeleteSource::None;
        Key key{};
        uint64_t next = 0;
    } deletion;
    void start_delete(DeleteSource source, const Key& key) {
        hold = Hold{};
        deletion = {source, key, clock + DeleteRepeat::delay};
        press(key); show_press(key);
    }
    void update_delete(unsigned pressed, unsigned held, bool touching, bool tapped, int x, int y) {
        if (deletion.source == DeleteSource::None) return;
        const Key& key = deletion.key;
        const bool down = deletion.source == DeleteSource::Touch ? touching &&
            x >= key.x && x < key.x + key.w && y >= key.y && y < key.y + key.h :
            deletion.source == DeleteSource::Square ? (held & SCE_CTRL_SQUARE) != 0 :
            (held & SCE_CTRL_CROSS) && !grid.empty() && grid[row][col].act == Act::Delete &&
                grid[row][col].x == key.x && grid[row][col].y == key.y;
        // Any new action ends the old gesture. Returning to a key cannot resume it.
        if (!down || pressed || tapped) { deletion = DeleteRepeat{}; feedback_down = false; return; }
        feedback_down = true;
        if (clock >= deletion.next) {
            if (cursor) { press(key); show_press(key); }
            // At most one repeat per frame: a delayed frame must not erase a burst of text.
            deletion.next = clock + DeleteRepeat::interval;
        }
    }
    size_t max_bytes = 1023;
    std::string text;
    size_t cursor = 0; // UTF-8 byte boundary; edits never split a code point.
    std::string context() const { return text.substr(0, cursor); }
    size_t previous() const {
        size_t at = cursor;
        if (at) --at;
        while (at && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) --at;
        return at;
    }
    void move_cursor(bool right) {
        if (right && cursor < text.size()) {
            ++cursor;
            while (cursor < text.size() && (static_cast<unsigned char>(text[cursor]) & 0xc0) == 0x80) ++cursor;
        } else if (!right) cursor = previous();
        casing.edited();
        feedback_down = false; feedback_until = clock + 140000;
    }
    struct Hold : KeyboardHold {
        bool touch = false;
        Key key{};
        int selected = 0, window = 0;
        uint64_t scroll_at = 0;
    } hold;
    struct Popup {
        int x, y, width, anchor;
        static constexpr int cell = 72, padding = 6, height = 66;
    };
    int popup_count() const {
        const int count = static_cast<int>(hold.key.alternatives.size());
        return hold.key.act == Act::Language ? std::min(count, 7) : count;
    }
    int popup_option(int cell, const Popup& popup) const {
        const int count = static_cast<int>(hold.key.alternatives.size());
        return hold.key.act == Act::Language ? hold.window + cell : (cell - popup.anchor + count) % count;
    }
    Popup popup_geometry() const {
        const int count = popup_count();
        const int width = count * Popup::cell + 2*Popup::padding;
        const int center = hold.key.x + hold.key.w / 2;
        const int x = std::max(8, std::min(952 - width, center - Popup::cell/2 - Popup::padding));
        const int anchor = std::max(0, std::min(count - 1, (center - x - Popup::padding) / Popup::cell));
        return {x, hold.key.y - 74, width, anchor};
    }
    void handle_hold(unsigned pressed, unsigned held, bool touching, int x, int y, uint64_t now) {
        const bool down = hold.touch ? touching : (held & SCE_CTRL_CROSS) != 0;
        const bool mature = hold.fired || now - hold.since >= KeyboardHold::delay;
        const unsigned cancel_buttons = SCE_CTRL_CIRCLE | SCE_CTRL_SELECT | SCE_CTRL_START |
            SCE_CTRL_TRIANGLE | SCE_CTRL_SQUARE | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
        const unsigned arrows = SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_UP | SCE_CTRL_DOWN;
        if ((pressed & cancel_buttons) || (hold.touch && (pressed & (arrows | SCE_CTRL_CROSS))) ||
            (!mature && (pressed & arrows))) {
            hold = Hold{}; feedback_down = false; feedback_until = 0;
            return;
        }
        const int count = static_cast<int>(hold.key.alternatives.size());
        if (hold.touch && down) {
            if (!mature && (x < hold.key.x || x >= hold.key.x + hold.key.w ||
                            y < hold.key.y || y >= hold.key.y + hold.key.h)) {
                hold = Hold{}; feedback_down = false; feedback_until = 0;
                return;
            }
            if (mature) {
                const Popup popup = popup_geometry();
                // The strip between the key and panel is part of the gesture, not a cancellation gap.
                if (x < popup.x || x >= popup.x + popup.width || y < popup.y || y >= hold.key.y + hold.key.h + 12) {
                    hold.selected = -1; hold.scroll_at = 0;
                } else {
                    const int visible = popup_count();
                    const int cell = std::max(0, std::min(visible - 1, (x - popup.x - Popup::padding) / Popup::cell));
                    // For larger language sets, dwell at an edge to scroll the carousel.
                    const int direction = hold.key.act != Act::Language ? 0 :
                        cell == visible - 1 && hold.window + visible < count ? 1 :
                        cell == 0 && hold.window > 0 ? -1 : 0;
                    if (!direction) hold.scroll_at = 0;
                    else if (!hold.scroll_at) hold.scroll_at = now + 350000;
                    else if (now >= hold.scroll_at) {
                        hold.window += direction; hold.scroll_at = now + 180000;
                    }
                    hold.selected = popup_option(cell, popup);
                }
            }
        } else if (!hold.touch && mature) {
            if (pressed & SCE_CTRL_LEFT) hold.selected = (hold.selected + count - 1) % count;
            if (pressed & SCE_CTRL_RIGHT) hold.selected = (hold.selected + 1) % count;
            if (hold.key.act == Act::Language) {
                hold.window = std::min(hold.window, hold.selected);
                if (hold.selected >= hold.window + popup_count()) hold.window = hold.selected - popup_count() + 1;
            }
        }
        hold.update(now, down, false);
        if (!down) {
            // A long press opens choices; only release commits the selected character.
            if (!mature) press(hold.key);
            else if (hold.selected >= 0) {
                const std::string chosen = hold.key.alternatives[hold.selected];
                if (hold.key.act == Act::Language) choose_language(chosen, true);
                else press({0, 0, 0, 0, Act::Char, chosen});
            }
            hold = Hold{}; feedback_down = false; feedback_until = 0;
        }
    }
    std::vector<std::vector<Key>> grid;
    void geometry() {
        const bool shifted = casing.upper(context());
        constexpr int keys_top = 262, key_h = 64, gap = 8;
        // Geometry is shared by rendering, controller focus and touch hit testing.
        grid.clear();
        const auto& rows = symbols ? layout().symbols : layout().rows;
        if (layout().system()) return;
        int y = keys_top;
        for (size_t r = 0; r < rows.size(); ++r) {
            const auto labels = keyboard_layout::characters(rows[r]);
            const bool last = r + 1 == rows.size();
            const int side = last ? 82 : 0;
            const int available = 928 - (last ? 2 * (side + gap) : 0);
            const int kw = std::min(88, (available - ((int)labels.size() - 1) * gap) / (int)labels.size());
            const int width = (int)labels.size() * (kw + gap) - gap + (last ? 2 * (side + gap) : 0);
            int x = 480 - width / 2;
            std::vector<Key> line;
            if (last) { line.push_back({x, y, side, key_h, Act::Shift, ""}); x += side + gap; }
            for (size_t k = 0; k < labels.size(); ++k) {
                line.push_back({x, y, kw, key_h, Act::Char,
                    !symbols && casing.explicitUpper() && layout().shifted.count(labels[k]) ? layout().shifted.at(labels[k]) :
                    shifted && !symbols ? keyboard_layout::upper(labels[k], code) : labels[k],
                    keyboard_layout::alternatives(layout(), labels[k], symbols ? 3 : r, k, shifted && !symbols)});
                x += kw + gap;
            }
            if (last) line.push_back({x, y, side, key_h, Act::Delete, ""});
            grid.push_back(std::move(line));
            y += key_h + gap;
        }
        std::vector<Key> line;
        int x = 16;
        auto add = [&](int width, Act act, const std::string& label = "") {
            line.push_back({x, y, width, key_h, act, label});
            x += width + gap;
            if (act == Act::Language) {
                auto choices = language_choices();
                const auto current = std::find_if(choices.begin(), choices.end(), [&](const keyboard_layout::Layout* profile) { return profile->code == code; });
                std::rotate(choices.begin(), current, choices.end());
                if (choices.size() > 1)
                    for (const auto* profile : choices) line.back().alternatives.push_back(profile->code);
            }
        };
        if (secret) {
            // Keep the vault's familiar controls and byte-preserving entry behavior.
            add(64, Act::Cancel);
            add(104, Act::Symbols, symbols ? "abc" : "?123");
            add(90, Act::Language, layout().badge);
            add(394, Act::Space);
            add(124, Act::Clear);
            add(112, Act::Ok, tr("vault.kb.ok"));
        } else {
            // Frequent punctuation beside a flexible spacebar; actions stay on the right.
            add(64, Act::Cancel);
            add(88, Act::Symbols, symbols ? "abc" : "?123");
            add(72, Act::Language, layout().badge);
            add(52, Act::Char, ",");
            line.back().alternatives = keyboard_layout::alternatives(layout(), ",", 3, 0, false);
            add(message ? 352 : 440, Act::Space);
            add(52, Act::Char, ".");
            line.back().alternatives = keyboard_layout::alternatives(layout(), ".", 3, 0, false);
            if (message) add(80, Act::Newline);
            add(112, Act::Ok, tr(message ? "keyboard.send" : "vault.kb.ok"));
        }
        grid.push_back(std::move(line));
        if (split_layout) {
            const int side_width=384-80*split_level;
            constexpr int gap=8;
            size_t largest=0;
            for (size_t r=0;r+1<grid.size();++r) largest=std::max(largest,(grid[r].size()+1)/2);
            const int width=std::min(64,(side_width-(int(largest)-1)*gap)/int(largest));
            for (size_t r=0;r+1<grid.size();++r) {
                const size_t middle=(grid[r].size()+1)/2;
                for (size_t k=0;k<grid[r].size();++k) {
                    auto& key=grid[r][k];key.w=width;
                    key.x=k<middle ? 16+int(k)*(width+gap) : 944-int(grid[r].size()-k)*(width+gap)+gap;
                }
            }
        }
        row = std::max(0, std::min(row, (int)grid.size() - 1));
        col = std::max(0, std::min(col, (int)grid[row].size() - 1));
    }
    bool can_submit() const { return (!secret && !message) || !text.empty(); }

    bool press(const Key& key) {
        switch (key.act) {
            case Act::Char:
                if (text.size() + key.label.size() <= max_bytes) {
                    text.insert(cursor, key.label);
                    cursor += key.label.size();
                    casing.inserted();
                }
                break;
            case Act::Newline:
                if (message && text.size() < max_bytes) { text.insert(cursor++, 1, '\n'); casing.edited(); }
                break;
            case Act::Space: if (text.size() < max_bytes) text.insert(cursor++, 1, ' '); casing.otherAction(); break;
            case Act::Shift: casing.shift(clock, context()); symbols = false; break;
            case Act::Symbols: symbols = !symbols; casing.otherAction(); break;
            case Act::Language: {
                const auto choices = language_choices();
                if (choices.size() > 1) {
                    size_t at = 0;
                    while (at < choices.size() && choices[at]->code != code) ++at;
                    const std::string next = choices[(at + 1) % choices.size()]->code;
                    choose_language(next, true);
                }
                break;
            }
            case Act::Cancel: cancel = true; break;
            case Act::Clear:
                if (secret) { std::fill(text.begin(), text.end(), '\0'); text.clear(); cursor = 0; casing.edited(); }
                break;
            case Act::Delete:
                if (cursor) {
                    const size_t at = previous();
                    text.erase(at, cursor - at); cursor = at;
                }
                casing.edited();
                break;
            case Act::Ok: return can_submit();
        }
        return false;
    }
    int contact_id = -1, contact_x = 0, contact_y = 0;
    std::array<bool, 256> previous_contacts{};
    struct TouchSample { SceTouchData data; uint64_t time; };
    std::vector<TouchSample> gesture_samples;
    TouchHistory touch_history;
    uint64_t input_clock=0;
    unsigned previous_held=0;
    Result handle(unsigned pressed, unsigned held, bool touch_down, int tx, int ty, uint64_t now, const SceTouchData* touches) {
        clock = now;
        if (!touches) return handle_keys(pressed, held, touch_down, tx, ty, now, false);
        const bool finished = gesture.finished;
        if (split_gesture(*touches,now)) {
            if (pressed & (SCE_CTRL_SELECT | SCE_CTRL_CIRCLE)) return Result::Cancelled;
            if (!finished && !gesture.finished) {
                // Only a completed split gesture may discard typing; replay failed candidates.
                if (touches->reportNum && gesture.armed && gesture_samples.size() < 128 && !pressed) {
                    gesture_samples.push_back({*touches,now});
                    return Result::None;
                }
                if (touches->reportNum) { gesture.captured=false; gesture.rejected=true; }
                for (const auto& sample : gesture_samples) {
                    const auto result=handle_contacts(0,held,sample.data,sample.time);
                    if (result != Result::None) { gesture_samples.clear(); return result; }
                }
                gesture_samples.clear();
                return handle_contacts(pressed,held,*touches,now);
            }
            gesture_samples.clear();
            hold=Hold{}; deletion=DeleteRepeat{}; pending_touch_active=false;
            feedback_down=false; feedback_until=0; was_touching=false;
            contact_id=-1;
            previous_contacts.fill(false);
            for (unsigned i=0;i<touches->reportNum;++i) previous_contacts[touches->report[i].id]=true;
            geometry();
            return Result::None;
        }
        return handle_contacts(pressed,held,*touches,now);
    }
    Result handle_contacts(unsigned pressed, unsigned held, const SceTouchData& data, uint64_t now) {
        const auto* touches=&data;
        const auto previous = previous_contacts;
        previous_contacts.fill(false);
        for (unsigned i = 0; i < touches->reportNum; ++i) previous_contacts[touches->report[i].id]=true;
        const SceTouchReport* current = nullptr;
        for (unsigned i = 0; i < touches->reportNum; ++i)
            if (touches->report[i].id == contact_id) current = &touches->report[i];
        if (current) { contact_x=current->x/2; contact_y=current->y/2; }
        bool handled = false;
        for (unsigned i = 0; i < touches->reportNum; ++i) {
            const auto& next = touches->report[i];
            if (previous[next.id]) continue;
            // A new contact ends the previous key even when no empty frame was sampled.
            if (contact_id >= 0) {
                const auto result=handle_keys(0,held,false,contact_x,contact_y,now,true);
                if (result != Result::None) return result;
            }
            contact_id=next.id; contact_x=next.x/2; contact_y=next.y/2;
            const auto result=handle_keys(handled ? 0 : pressed,held,true,contact_x,contact_y,now,true);
            handled=true;
            if (result != Result::None) return result;
        }
        if (handled) return Result::None;
        const auto result=handle_keys(pressed,held,current != nullptr,contact_x,contact_y,now,true);
        if (!current) contact_id=-1;
        return result;
    }
    Result handle_keys(unsigned pressed, unsigned held, bool touch_down, int tx, int ty, uint64_t now, bool defer_touch) {
        clock = now;
        if (pending_touch_active) {
            const auto key=pending_touch;
            const bool inside=tx>=key.x && tx<key.x+key.w && ty>=key.y && ty<key.y+key.h;
            if (pressed || (touch_down && !inside) || now<pending_touch_since) pending_touch_active=false;
            else if (!touch_down) {
                pending_touch_active=false; was_touching=false; feedback_down=false;
                const bool submit=press(key);
                geometry();
                return cancel ? Result::Cancelled : submit ? Result::Submitted : Result::None;
            } else if (key.act==Act::Delete && now-pending_touch_since>=DeleteRepeat::delay) {
                pending_touch_active=false; start_delete(DeleteSource::Touch,key);
                deletion.next=now+DeleteRepeat::interval;
            } else return Result::None;
        }
        feedback_down = touch_mode ? touch_down && tx >= feedback.x && tx < feedback.x + feedback.w &&
            ty >= feedback.y && ty < feedback.y + feedback.h : (held & SCE_CTRL_CROSS) != 0;
        geometry();
        const bool tapped = touch_down && !was_touching;
        was_touching = touch_down;
        if (layout().system()) { deletion = DeleteRepeat{}; return Result::System; }
        update_delete(pressed, held, touch_down, tapped, tx, ty);
        if (hold.active) {
            handle_hold(pressed, held, touch_down, tx, ty, now);
            geometry();
            return layout().system() ? Result::System : Result::None;
        }
        if (pressed & (SCE_CTRL_SELECT | SCE_CTRL_CIRCLE)) return Result::Cancelled;
        bool submit = (pressed & SCE_CTRL_START) && can_submit();
        const int focus_x = grid[row][col].x + grid[row][col].w / 2;
        if (pressed & SCE_CTRL_UP) row = (row + (int)grid.size() - 1) % (int)grid.size();
        if (pressed & SCE_CTRL_DOWN) row = (row + 1) % (int)grid.size();
        if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
            col = 0;
            for (int k = 1; k < (int)grid[row].size(); ++k)
                if (std::abs(grid[row][k].x + grid[row][k].w / 2 - focus_x) <
                    std::abs(grid[row][col].x + grid[row][col].w / 2 - focus_x)) col = k;
        }
        if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS))
            touch_mode = false;
        if (pressed & SCE_CTRL_LEFT) col = (col + (int)grid[row].size() - 1) % (int)grid[row].size();
        if (pressed & SCE_CTRL_RIGHT) col = (col + 1) % (int)grid[row].size();
        bool deleted = false;
        auto erase = [&](DeleteSource source, const Key& key) {
            if (deleted) return;
            deleted = true;
            start_delete(source, key);
        };
        auto activate = [&](const Key& key, bool by_touch) {
            hold.active = false;
            if (key.act != Act::Delete) deletion = DeleteRepeat{};
            touch_mode = by_touch;
            show_press(key);
            if (key.act != Act::Shift) casing.otherAction();
            if (key.act == Act::Delete) erase(by_touch ? DeleteSource::Touch : DeleteSource::Cross, key);
            else if ((key.act == Act::Char || key.act == Act::Language) && !key.alternatives.empty()) {
                hold.start(now); hold.touch = by_touch; hold.key = key;
                hold.selected = 0;
            } else submit = press(key) || submit;
        };
        if (pressed & SCE_CTRL_CROSS) activate(grid[row][col], false);
        for (const auto& line : grid) for (const auto& key : line) {
            if ((pressed & SCE_CTRL_SQUARE) && key.act == Act::Delete) erase(DeleteSource::Square, key);
            if ((pressed & SCE_CTRL_TRIANGLE) && key.act == Act::Space) {
                show_press(key);
                feedback_down = false;
            }
        }
        if (pressed & SCE_CTRL_TRIANGLE) press({0, 0, 0, 0, Act::Space, ""});
        if (pressed & SCE_CTRL_LTRIGGER) move_cursor(false);
        if (pressed & SCE_CTRL_RTRIGGER) move_cursor(true);
        if (tapped) {
            for (int r = 0; r < (int)grid.size(); ++r)
                for (int k = 0; k < (int)grid[r].size(); ++k) {
                    const Key& key = grid[r][k];
                    if (tx >= key.x && tx < key.x + key.w && ty >= key.y && ty < key.y + key.h) {
                        row = r; col = k;
                        // Immediate touch actions commit on release, so a second finger can claim the gesture.
                        if (defer_touch && !((key.act==Act::Char || key.act==Act::Language) && !key.alternatives.empty())) {
                            pending_touch=key; pending_touch_active=true; pending_touch_since=now;
                            touch_mode=true; show_press(key);
                        } else activate(key, true);
                    }
                }
        }
        if (cancel) return Result::Cancelled;
        if (submit) return Result::Submitted;
        geometry(); // Layout changes are visible in the same frame.
        return Result::None;
    }
    struct DraftLayout {
        std::string text;
        size_t cursor = 0, cursor_line = 0;
        int cursor_width = 0;
        vita2d_pgf* font = nullptr;
        unsigned revision = 0;
        std::vector<std::string> lines;
    };
    mutable DraftLayout draft;
    const DraftLayout& draft_layout(vita2d_pgf* font) const {
        const auto revision=rich_text_revision();
        if (!draft.lines.empty() && draft.text==text && draft.cursor==cursor &&
            draft.font==font && draft.revision==revision) return draft;
        draft={text,cursor,0,0,font,revision,{""}};
        for (size_t i=0;i<text.size();) {
            size_t n=1;
            while (i+n<text.size() && (static_cast<unsigned char>(text[i+n])&0xc0)==0x80) ++n;
            const auto glyph=text.substr(i,n);
            if (glyph!="\n" && rich_text_width(font,0.65f,(draft.lines.back()+glyph).c_str())>868)
                draft.lines.emplace_back();
            if (i==cursor) {
                draft.cursor_line=draft.lines.size()-1;
                draft.cursor_width=rich_text_width(font,0.65f,draft.lines.back().c_str());
            }
            if (glyph=="\n") draft.lines.emplace_back();
            else draft.lines.back()+=glyph;
            i+=n;
        }
        if (cursor==text.size()) {
            draft.cursor_line=draft.lines.size()-1;
            draft.cursor_width=rich_text_width(font,0.65f,draft.lines.back().c_str());
        }
        return draft;
    }
    void draw(vita2d_pgf* font, bool dark, const std::string& title, bool keyboard_only) const {
        performance::SlowScope text_draw(performance::SlowPhase::Ui,"keyboard_text");
        const Colors c = colors(dark);
        const bool shifted = casing.upper(context());
        if (!keyboard_only) {
            vita2d_draw_rectangle(0, 0, 960, 544, c.background | 0xff000000u);
            ui_skin::background();
            const bool header = ui_skin::header();
            const int title_width = rich_text_width(font, 0.74f, title.c_str());
            const float title_scale = title_width > 728 ? 0.74f * 728 / title_width : 0.74f;
            rich_text_draw(font, 28, 28, header ? c.header_ink : c.ink, title_scale, title.c_str());
            // A quiet layout badge keeps the active language visible while typing.
            const std::string layout_label = layout().badge + (symbols ? " / ?123" : casing.caps() ? " / CAPS" : shifted ? " / ABC" : " / abc");
            rounded_key_base(784, 4, 148, 28, 10, c.utility);
            rich_text_draw(font, 858 - rich_text_width(font, 0.48f, layout_label.c_str()) / 2, 25, c.utility_ink, 0.48f, layout_label.c_str());
            if (!secret) {
                const std::string help = tr("keyboard.help");
                const int help_width = rich_text_width(font, 0.44f, help.c_str());
                const float help_scale = help_width > 904 ? 0.44f * 904 / help_width : 0.44f;
                rich_text_draw(font, 28, 63, c.muted, help_scale, help.c_str());
                rounded_key_base(20, 78, 920, 164, 12, c.separator);
                rounded_key_base(22, 80, 916, 160, 10, c.key);
                const auto& layout=draft_layout(font);
                const auto& lines=layout.lines;
                const auto cursor_line=layout.cursor_line;
                const auto cursor_width=layout.cursor_width;
                // Keep the insertion point visible when navigating a long draft.
                const size_t first = cursor_line > 4 ? cursor_line - 4 : 0;
                const size_t last = std::min(lines.size(), first + 5);
                for (size_t i = first; i < last; ++i)
                    rich_text_draw(font, 36, 106 + (i-first)*26, c.key_ink, 0.65f, lines[i].c_str());
                if ((clock / 500000) % 2 == 0 || clock < feedback_until)
                    vita2d_draw_rectangle(38 + cursor_width, 87 + (cursor_line-first)*26, 2, 22, c.accent);
                const std::string counter = std::to_string(count_chars(text));
                rich_text_draw(font, 912 - rich_text_width(font, 0.42f, counter.c_str()), 234, c.key_muted, 0.42f, counter.c_str());
                // Indicate hidden earlier lines without confusing them with message content.
                if (first) for (int i = 0; i < 3; ++i) vita2d_draw_fill_circle(465 + i*10, 86, 2, c.muted);
            }
            if (secret) {
                const size_t count = count_chars(text);
                const size_t before = count_chars(context());
                const size_t first = before > 32 ? before - 32 : 0;
                const int shown = static_cast<int>(std::min<size_t>(count - first, 32));
                for (int i = 0; i < shown; ++i)
                    vita2d_draw_fill_circle(480 + (i-(shown-1)/2.0f)*22, 216, 8, c.accent);
                if ((clock / 500000) % 2 == 0 || clock < feedback_until)
                    vita2d_draw_rectangle(480 - shown*11 + (before-first)*22, 205, 2, 22, c.accent);
            }
        }
        text_draw.finish();
        performance::SlowScope keys_draw(performance::SlowPhase::Ui,"keyboard_keys");
        const bool trace_keys=performance::enabled && diag_log::enabled(diag_log::Kind::Performance);
        auto stamp=[&] { return trace_keys ? performance::now() : int64_t(0); };
        int64_t faces_us=0,theme_us=0,labels_us=0,hints_us=0;
        vita2d_draw_rectangle(0, 250, 960, 294, c.panel);
        vita2d_draw_rectangle(0, 250, 960, 1, c.separator);
        for (int r = 0; r < (int)grid.size(); ++r)
            for (int k = 0; k < (int)grid[r].size(); ++k) {
                const Key& key = grid[r][k];
                const bool selected = !touch_mode && r == row && k == col;
                const bool pressed = key.x == feedback.x && key.y == feedback.y &&
                    (feedback_down || clock < feedback_until);
                const bool ok = key.act == Act::Ok;
                const bool disabled = ok && !can_submit();
                const bool toggled = (key.act == Act::Shift && shifted && !symbols) ||
                                     (key.act == Act::Symbols && symbols);
                const bool utility = key.act != Act::Char;
                const bool locked = key.act == Act::Shift && casing.caps();
                const unsigned fill = locked && !symbols ? c.accent : disabled ? c.utility : pressed || toggled ? c.active :
                    ok ? c.accent : utility ? c.utility : c.key;
                const unsigned normal_ink = fill == c.accent ? c.on_accent : fill == c.active ? c.active_ink : fill == c.utility ? c.utility_ink : c.key_ink;
                const unsigned hint_ink = fill == c.accent ? c.on_accent : fill == c.active ? c.active_muted : fill == c.utility ? c.utility_muted : c.key_muted;
                const unsigned color = disabled ? hint_ink : normal_ink;
                const int inset = pressed ? 2 : 0;
                const int radius = split_layout && (ok || key.act==Act::Symbols) ? 28 : 10;
                // Soft key faces, with one precise controller focus outline.
                auto part=stamp();
                rounded_key_base(key.x - 2, key.y - 2, key.w + 4, key.h + 4, radius + 2, selected ? c.focus : c.separator);
                rounded_key_base(key.x + inset, key.y + inset, key.w - 2*inset, key.h - 2*inset, radius, fill);
                auto end=stamp(); faces_us+=end-part; part=end;
                if (fill == c.key || fill == c.active)
                    ui_skin::nine(fill == c.active ? ui_skin::Slice::RowFocused : ui_skin::Slice::Row,
                        key.x + inset + 2, key.y + inset + 2, key.w - 2*inset - 4, key.h - 2*inset - 4);
                end=stamp(); theme_us+=end-part; part=end;
                const float cx = key.x + key.w / 2.0f;
                const float cy = key.y + key.h / 2.0f;
                if (key.act == Act::Shift) {
                    triangle(cx, cy - 4, 7, 0, color);
                    vita2d_draw_rectangle(cx - 4, cy + 2, 8, 10, color);
                    if (casing.caps()) vita2d_draw_rectangle(cx - 10, cy + 17, 20, 3, color);
                } else if (key.act == Act::Delete) {
                    // Backspace outline and cross, independent of font coverage.
                    vita2d_draw_line(cx - 18, cy, cx - 8, cy - 10, color);
                    vita2d_draw_line(cx - 18, cy, cx - 8, cy + 10, color);
                    vita2d_draw_line(cx - 8, cy - 10, cx + 17, cy - 10, color);
                    vita2d_draw_line(cx - 8, cy + 10, cx + 17, cy + 10, color);
                    vita2d_draw_line(cx + 17, cy - 10, cx + 17, cy + 10, color);
                    draw_symbol(4, cx + 4, cy, 6, color, fill);
                } else if (key.act == Act::Clear) {
                    // Trash can: vault-only clear action, without a shoulder-button shortcut.
                    vita2d_draw_line(cx - 10, cy - 8, cx + 10, cy - 8, color);
                    vita2d_draw_line(cx - 6, cy - 12, cx + 6, cy - 12, color);
                    vita2d_draw_line(cx - 8, cy - 6, cx - 6, cy + 12, color);
                    vita2d_draw_line(cx + 8, cy - 6, cx + 6, cy + 12, color);
                    vita2d_draw_line(cx - 6, cy + 12, cx + 6, cy + 12, color);
                    vita2d_draw_line(cx - 3, cy - 3, cx - 3, cy + 8, color);
                    vita2d_draw_line(cx + 3, cy - 3, cx + 3, cy + 8, color);
                } else if (key.act == Act::Cancel) {
                    // Downward chevron: dismiss the keyboard.
                    vita2d_draw_line(cx - 10, cy - 4, cx, cy + 6, color);
                    vita2d_draw_line(cx, cy + 6, cx + 10, cy - 4, color);
                } else if (key.act == Act::Newline) {
                    // Draw Return directly so its shape doesn't depend on font coverage.
                    vita2d_draw_line(cx + 13, cy - 9, cx + 13, cy + 3, color);
                    vita2d_draw_line(cx + 13, cy + 3, cx - 12, cy + 3, color);
                    vita2d_draw_line(cx - 12, cy + 3, cx - 5, cy - 4, color);
                    vita2d_draw_line(cx - 12, cy + 3, cx - 5, cy + 10, color);
                } else {
                    std::string label = key.act == Act::Space ? layout().name : key.label;
                    float scale = key.act == Act::Char ? 0.82f : key.act == Act::Space ? 0.56f : 0.62f;
                    int w = rich_text_width(font, scale, label.c_str());
                    if (w > key.w - 16) { scale *= (key.w - 16.0f) / w; w = rich_text_width(font, scale, label.c_str()); }
                    rich_text_draw(font, key.x + (key.w - w) / 2, cy + 9, color, scale, label.c_str());
                    if (key.act == Act::Space) {
                        vita2d_draw_line(cx - 16, cy + 20, cx + 16, cy + 20, hint_ink);
                    }
                }
                end=stamp(); labels_us+=end-part; part=end;
                if (secret) {
                    const unsigned badge_color = color;
                    const int shortcut = key.act == Act::Delete ? 1 : key.act == Act::Space ? 0 :
                        key.act == Act::Cancel ? 3 : key.act == Act::Ok ? 5 : -1;
                    if (shortcut >= 0) shortcut_badge(font, shortcut, key.x + key.w - 8, key.y + 7, badge_color, fill);
                    if (key.act == Act::Space) {
                        shortcut_badge(font, 7, key.x + 60, key.y + 7, badge_color, fill);
                        triangle(key.x + 74, key.y + 14, 4, 2, badge_color);
                        triangle(key.x + key.w - 74, key.y + 14, 4, 3, badge_color);
                        shortcut_badge(font, 8, key.x + key.w - 45, key.y + 7, badge_color, fill);
                    }
                }
                if (!key.alternatives.empty()) {
                    std::string hint;
                    for (size_t i = 0; i < std::min<size_t>(2, key.alternatives.size()); ++i) {
                        if (!hint.empty()) hint += " ";
                        hint += key.alternatives[i];
                    }
                    if (key.alternatives.size() > 2) hint += " …";
                    if (key.act == Act::Language) hint = "…";
                    float scale = 0.36f;
                    int width = rich_text_width(font, scale, hint.c_str());
                    if (width > key.w - 12) { scale *= float(key.w - 12) / width; width = rich_text_width(font, scale, hint.c_str()); }
                    rich_text_draw(font, key.x + key.w - width - 7, key.y + 15,
                        hint_ink,
                        scale, hint.c_str());
                }
                hints_us+=stamp()-part;
            }
        if (trace_keys) {
            char detail[96];
            std::snprintf(detail,sizeof(detail),"keyboard_keys_f%lld_s%lld_l%lld_h%lld",
                (long long)faces_us,(long long)theme_us,(long long)labels_us,(long long)hints_us);
            keys_draw.name(detail);
        }
        keys_draw.finish();
        performance::SlowScope preview_draw(performance::SlowPhase::Ui,"keyboard_preview");
        if (hold.active && hold.fired) {
            const Popup popup = popup_geometry();
            const int count = popup_count();
            rounded_key_base(popup.x, popup.y + 3, popup.width, Popup::height, 16, c.separator);
            rounded_key_base(popup.x, popup.y, popup.width, Popup::height, 16, c.utility);
            // These are available keyboard choices, never the entered secret. Secrets get no enlarged preview.
            for (int cell = 0; cell < count; ++cell) {
                const int option = popup_option(cell, popup);
                const int x = popup.x + Popup::padding + cell*Popup::cell;
                const bool selected = option == hold.selected;
                if (selected) rounded_key_base(x, popup.y + 6, Popup::cell, 54, 12, c.accent);
                const bool language = hold.key.act == Act::Language;
                const std::string label = language ? keyboard_layout::get(hold.key.alternatives[option]).badge : hold.key.alternatives[option];
                const float scale = language ? 0.68f : secret ? 0.8f : 1.05f;
                rich_text_draw(font, x + (Popup::cell - rich_text_width(font, scale, label.c_str())) / 2,
                    popup.y + 43, selected ? c.on_accent : c.utility_ink, scale, label.c_str());
            }
            if (hold.key.act == Act::Language && hold.key.alternatives.size() > static_cast<size_t>(count)) {
                if (hold.window > 0) triangle(popup.x + 4, popup.y + 33, 3, 2, c.accent);
                if (hold.window + count < (int)hold.key.alternatives.size()) triangle(popup.x + popup.width - 4, popup.y + 33, 3, 3, c.accent);
            }
        } else if (!secret && feedback.act == Act::Char && (feedback_down || clock < feedback_until)) {
            const std::string& label = feedback.label;
            const int width = std::max(72, feedback.w);
            const int x = std::max(8, std::min(952 - width, feedback.x + (feedback.w - width) / 2));
            const int y = feedback.y - 72;
            rounded_key_base(x, y + 3, width, 66, 16, c.separator);
            rounded_key_base(x, y, width, 66, 16, c.active);
            rich_text_draw(font, x + (width - rich_text_width(font, 1.05f, label.c_str())) / 2,
                y + 44, c.active_ink, 1.05f, label.c_str());
            if (hold.active) {
                const float progress = std::min(1.0f, float(clock - hold.since) / KeyboardHold::delay);
                vita2d_draw_rectangle(x + 14, y + 57, (width - 28)*progress, 2, c.accent);
            }
        }

    }
};

CustomKeyboard::CustomKeyboard() : impl_(new Impl) {}
CustomKeyboard::~CustomKeyboard() { clear(); }
void CustomKeyboard::clear() {
    impl_->gesture = Impl::Gesture{}; impl_->pending_touch_active=false;
    impl_->contact_id=-1; impl_->previous_contacts.fill(false);
    impl_->gesture_samples.clear();
    impl_->touch_history.ignore(nullptr); impl_->input_clock=0; impl_->previous_held=0;
    impl_->draft=Impl::DraftLayout{};
    std::fill(impl_->text.begin(), impl_->text.end(), '\0'); impl_->text.clear(); impl_->cursor = 0;
    impl_->hold = Impl::Hold{};
    impl_->deletion = Impl::DeleteRepeat{};
    impl_->feedback_down = false; impl_->feedback_until = 0;
}
void CustomKeyboard::open(const std::string& initial, bool secret, bool message, size_t max_bytes, bool allow_system, bool auto_capitalize) {
    const std::string language = impl_->language_chosen ? impl_->code : keyboard_languages::canonical(i18n::systemLanguage());
    const bool chosen = impl_->language_chosen;
    const bool split = impl_->split_layout;
    const int split_level = impl_->split_level;
    const auto preferences = impl_->preferences;
    clear();
    *impl_ = Impl{};
    impl_->split_layout = split;
    impl_->split_level = split_level;
    impl_->allow_system = allow_system;
    impl_->preferences = preferences;
    impl_->apply_languages();
    impl_->auto_capitalize = auto_capitalize;
    impl_->code = keyboard_languages::contains(impl_->enabled, language) ? language : impl_->enabled.front();
    impl_->language_chosen = chosen;
    impl_->secret = secret; impl_->message = message; impl_->max_bytes = max_bytes;
    // A secret never reallocates while typed: a grown string would leave the old prefix in freed memory.
    if (secret) impl_->text.reserve(max_bytes);
    for (size_t i = 0; i < initial.size();) {
        size_t n = 1;
        while (i+n < initial.size() && (static_cast<unsigned char>(initial[i+n]) & 0xc0) == 0x80) ++n;
        if (impl_->text.size() + n > max_bytes) break;
        impl_->text.append(initial, i, n); i += n;
    }
    impl_->cursor = impl_->text.size();
    impl_->casing.reset(auto_capitalize && message && !secret);
    impl_->geometry();
}
CustomKeyboard::Result CustomKeyboard::handle(unsigned pressed, unsigned held, bool touch_down, int x, int y, uint64_t now, const SceTouchData* touches) {
    if (touches && touches->reportNum>SCE_TOUCH_MAX_REPORT) return Result::None;
    const auto count=impl_->touch_history.poll(touches,[](SceTouchData* data,unsigned size) {
        return sceTouchRead(SCE_TOUCH_PORT_FRONT,data,size);
    });
    const auto previous_held=impl_->previous_held;
    impl_->previous_held=held;
    if (!count) {
        impl_->input_clock=std::max(impl_->input_clock,now);
        return impl_->handle(pressed,held,touch_down,x,y,impl_->input_clock,touches);
    }
    const uint64_t latest=impl_->touch_history.samples[count-1].timeStamp;
    for (unsigned i=0;i<count;++i) {
        const auto& sample=impl_->touch_history.samples[i];
        const auto age=std::min<uint64_t>(now,latest-sample.timeStamp);
        impl_->input_clock=std::max(impl_->input_clock,now-age);
        const bool last=i+1==count;
        const auto result=impl_->handle(last ? pressed : 0,last ? held : previous_held,
            sample.reportNum!=0,sample.report[0].x/2,sample.report[0].y/2,impl_->input_clock,&sample);
        if (result!=Result::None) return result;
    }
    return Result::None;
}
void CustomKeyboard::ignoreTouchHistory(const SceTouchData* touches) { impl_->touch_history.ignore(touches); }
void CustomKeyboard::draw(vita2d_pgf* font, bool dark, const std::string& title, bool keyboard_only) const { impl_->draw(font, dark, title, keyboard_only); }
const std::string& CustomKeyboard::text() const { return impl_->text; }
bool CustomKeyboard::split() const { return impl_->split_layout; }
size_t CustomKeyboard::cursor() const { return impl_->cursor; }

void CustomKeyboard::setSplitLevel(int level) {
    level = std::max(0, std::min(2, level));
    if (impl_->split_level == level) return;
    impl_->split_level = level;
    impl_->geometry();
}

void CustomKeyboard::setLanguages(const std::vector<std::string>& codes) {
    impl_->preferences = keyboard_languages::normalize(codes);
    impl_->apply_languages();
    if (!keyboard_languages::contains(impl_->enabled, impl_->code)) {
        impl_->code = impl_->enabled.front(); impl_->language_chosen = false;
        impl_->hold = Impl::Hold{}; impl_->deletion = Impl::DeleteRepeat{};
    }
    impl_->geometry();
}
bool CustomKeyboard::setLanguage(const std::string& code) { return impl_->choose_language(code); }
const std::string& CustomKeyboard::language() const { return impl_->code; }
bool CustomKeyboard::systemRequested() const { return impl_->layout().system(); }
