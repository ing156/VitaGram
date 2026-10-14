#include "vault_ui.h"
#include "keyboard_hold.h"
#include "account_cleanup.h"
#include "i18n.h"
#include "media_render.h"
#include "vault.h"
#include "vault_sweep.h"
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/threadmgr.h>
#include <sys/stat.h>
#include <ctime>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "ui_skin.h"
#include <string>
#include <vector>

namespace vault_ui {
namespace {
struct Colors {
    unsigned background, panel, ink, muted, accent, danger, ok, warn, separator;
};

Colors colors(bool dark) {
    Colors c;
    c.background = dark ? RGBA8(16, 24, 33, 255) : RGBA8(238, 243, 248, 255);
    c.panel = dark ? RGBA8(31, 45, 59, 255) : RGBA8(255, 255, 255, 255);
    c.ink = dark ? RGBA8(231, 238, 245, 255) : RGBA8(39, 51, 66, 255);
    c.muted = dark ? RGBA8(163, 183, 200, 255) : RGBA8(105, 122, 140, 255);
    c.accent = RGBA8(35, 135, 210, 255);
    c.danger = RGBA8(214, 69, 65, 255);
    c.ok = RGBA8(20, 143, 82, 255);
    c.warn = RGBA8(224, 146, 36, 255);
    c.separator = dark ? RGBA8(53, 70, 86, 255) : RGBA8(205, 216, 226, 255);
    // Screens sit on a themed card: hollow marks must be filled with the card color.
    if (ui_skin::ready()) c.background = c.panel = ui_skin::surface_color(c.panel);
    return c;
}

// Keys in the order of vault::Pin symbols.
const unsigned kSymbolButtons[vault::kSymbols] = {
    SCE_CTRL_UP, SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT,
    SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_TRIANGLE, SCE_CTRL_SQUARE};
constexpr unsigned kAllSymbolButtons = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT |
                                       SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE | SCE_CTRL_SQUARE;

// Edge-detected controller state shared by all screens.
struct Pad {
    unsigned old = 0;
    Pad() { SceCtrlData d{}; sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG); sceCtrlPeekBufferPositive(0, &d, 1); old = d.buttons; }
    unsigned poll() {
        SceCtrlData d{};
        sceCtrlPeekBufferPositive(0, &d, 1);
        unsigned pressed = d.buttons & ~old;
        old = d.buttons;
        return pressed;
    }
};

// ---- drawing -------------------------------------------------------------------------------
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

// Opaque rounded key bases keep a visible rim even when the PNG has transparent edges.
void rounded_key_base(float x, float y, float w, float h, float radius, unsigned color) {
    vita2d_draw_rectangle(x + radius, y, w - 2 * radius, h, color);
    vita2d_draw_rectangle(x, y + radius, w, h - 2 * radius, color);
    for (float cx : {x + radius, x + w - radius})
        for (float cy : {y + radius, y + h - radius})
            vita2d_draw_fill_circle(cx, cy, radius, color);
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

std::vector<std::string> wrap(vita2d_pgf* font, const std::string& text, int width, float scale) {
    auto measure = [&](const std::string& value) { return rich_text_width(font, scale, value.c_str()); };
    std::vector<std::string> lines;
    size_t start = 0;
    for (;;) {
        size_t end = text.find('\n', start);
        std::string paragraph = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        std::string line;
        size_t i = 0;
        while (i < paragraph.size()) {
            size_t space = paragraph.find(' ', i);
            std::string word = paragraph.substr(i, space == std::string::npos ? std::string::npos : space - i);
            i = space == std::string::npos ? paragraph.size() : space + 1;
            if (word.empty()) continue;
            std::string trial = line.empty() ? word : line + " " + word;
            if (measure(trial) <= width) { line = trial; continue; }
            if (!line.empty()) { lines.push_back(line); line.clear(); }
            if (measure(word) <= width) { line = word; continue; }
            // Longer than a whole line (or text without spaces, e.g. Japanese): break between characters.
            for (size_t k = 0; k < word.size();) {
                size_t n = 1;
                while (k + n < word.size() && (static_cast<unsigned char>(word[k + n]) & 0xc0) == 0x80) ++n;
                std::string glyph = word.substr(k, n);
                if (!line.empty() && measure(line + glyph) > width) { lines.push_back(line); line.clear(); }
                line += glyph; k += n;
            }
        }
        lines.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

// Set by relock(): the app's own frame work, run while a lock screen owns the display.
void (*g_tick)(void*) = nullptr;
void* g_tick_context = nullptr;
bool g_deriving = false;

void begin_frame(const Env& env, const std::string& title, const std::string& footer) {
    if (g_tick && !g_deriving) g_tick(g_tick_context);
    ui_skin::load(env.theme >= 0 ? env.theme : (env.dark ? 1 : 0));   // no-op once this theme is loaded; the gate runs before the main loop
    Colors c = colors(env.dark);
    vita2d_set_clear_color(c.background);
    vita2d_start_drawing();
    vita2d_clear_screen();
    // Same wallpaper, header and panel PNGs as the main screens; flat shapes remain the fallback.
    ui_skin::background();
    if (!ui_skin::header(36)) vita2d_draw_rectangle(0, 0, 960, 36, env.dark ? c.panel : c.accent);
    rich_text_draw(env.font, 20, 25, RGBA8(240, 247, 255, 255), 0.65f, title.c_str());
    const unsigned int tint = RGBA8(255, 255, 255, 235);
    ui_skin::nine(ui_skin::Slice::Panel, 20, 48, 920, 444, tint);
    if (!ui_skin::nine(ui_skin::Slice::Panel, 4, 502, 952, 40, tint)) {
        vita2d_draw_rectangle(0, 500, 960, 44, c.panel);
        vita2d_draw_rectangle(0, 499, 960, 1, c.separator);
    }
    if (!footer.empty()) rich_text_draw(env.font, 28, 528, c.ink, 0.6f, footer.c_str());
}

void end_frame(bool with_dialog = false) {
    vita2d_end_drawing();
    if (with_dialog) {
        // The system keyboard renders into the same surfaces: finish our scene first.
        vita2d_wait_rendering_done();
        vita2d_common_dialog_update();
        vita2d_swap_buffers();
        sceGxmDisplayQueueFinish();
        return;
    }
    vita2d_swap_buffers();
}

int draw_lines(const Env& env, const std::string& text, int x, int y, int width, float scale, unsigned color, int step = 27) {
    for (const auto& line : wrap(env.font, text, width, scale)) {
        rich_text_draw(env.font, x, y, color, scale, line.c_str());
        y += step;
    }
    return y;
}

void progress_bar(const Env& env, int x, int y, int w, float fraction, unsigned color) {
    Colors c = colors(env.dark);
    vita2d_draw_rectangle(x, y, w, 8, c.separator);
    vita2d_draw_rectangle(x, y, w * std::max(0.0f, std::min(1.0f, fraction)), 8, color);
}

// ---- key derivation progress -------------------------------------------------------------------
void derive_progress(void* opaque, int percent) {
    g_deriving = true; // the key derivation must not re-enter the caller's event processing
    struct Reset { ~Reset() { g_deriving = false; } } reset;
    const Env& env = *static_cast<const Env*>(opaque);
    Colors c = colors(env.dark);
    begin_frame(env, tr("vault.title"), "");
    rich_text_draw(env.font, 48, 250, c.ink, 0.8f, tr("vault.deriving", {std::to_string(percent)}).c_str());
    progress_bar(env, 48, 275, 864, percent / 100.0f, c.accent);
    end_frame();
}

std::string result_text(vault::Result r) {
    switch (r) {
        case vault::Result::WrongPin: return tr("vault.wrong");
        case vault::Result::TooShort: return tr("vault.weak.short", {std::to_string(vault::kMinPin)});
        case vault::Result::Unavailable: return tr("vault.unavailable");
        case vault::Result::NoMemory: return tr("vault.no_memory");
        default: return tr("vault.error");
    }
}

// ---- key entry ---------------------------------------------------------------------------------
enum class End { Submit, Select };

struct Entry {
    std::string title, hint, footer, message;
    unsigned message_color = 0;
    bool show_strength = false;
};

End read_pin(const Env& env, Pad& pad, const Entry& entry, vault::Pin& pin) {
    Colors c = colors(env.dark);
    pin.clear();
    std::string notice;
    for (;;) {
        unsigned pressed = pad.poll();
        if (pressed & SCE_CTRL_SELECT) return End::Select;
        if (pressed & SCE_CTRL_START) {
            if (!pin.empty()) return End::Submit;
            notice = tr("vault.empty");
        }
        if (pressed & SCE_CTRL_LTRIGGER) { if (!pin.empty()) pin.pop_back(); notice.clear(); }
        if (pressed & SCE_CTRL_RTRIGGER) { pin.clear(); notice.clear(); }
        unsigned fresh = pressed & kAllSymbolButtons;
        if (fresh) {
            // Two buttons in the same frame (a slipped D-pad diagonal) would make the key
            // impossible to repeat reliably: ignore that frame.
            if (fresh & (fresh - 1)) notice = tr("vault.one_button");
            else if (pin.size() >= vault::kMaxPin) notice = tr("vault.too_long", {std::to_string(vault::kMaxPin)});
            else {
                for (int s = 0; s < vault::kSymbols; ++s) if (fresh == kSymbolButtons[s]) pin.push_back((uint8_t)s);
                notice.clear();
            }
        }

        begin_frame(env, entry.title, entry.footer);
        int y = draw_lines(env, entry.hint, 48, 72, 864, 0.6f, c.ink);
        // The key is never shown: one dot per press.
        const int per_row = 16, spacing = 40;
        const int rows = std::max<int>(1, ((int)pin.size() + per_row - 1) / per_row);
        const int top = std::max(y + 20, 190);
        for (int row = 0; row < rows; ++row) {
            int count = std::min<int>(per_row, std::max<int>(0, (int)pin.size() - row * per_row));
            int slots = pin.empty() ? 1 : (row == rows - 1 ? count : per_row);
            float x0 = 480 - (slots - 1) * spacing / 2.0f;
            for (int i = 0; i < slots; ++i) {
                float x = x0 + i * spacing, yy = top + row * 34;
                if (i < count) vita2d_draw_fill_circle(x, yy, 10, c.accent);
                else { vita2d_draw_fill_circle(x, yy, 10, c.separator); vita2d_draw_fill_circle(x, yy, 7, c.panel); }
            }
        }
        int below = top + rows * 34 + 18;
        if (entry.show_strength) {
            const int bits = vault::pin_strength_bits(pin);
            std::string line = tr("vault.strength", {std::to_string(pin.size()), std::to_string(bits)});
            int w = rich_text_width(env.font, 0.58f, line.c_str());
            rich_text_draw(env.font, 480 - w / 2, below + 14, c.muted, 0.58f, line.c_str());
            unsigned bar = bits < 24 ? c.danger : bits < 36 ? c.warn : c.ok;
            progress_bar(env, 300, below + 26, 360, bits / 48.0f, bar);
            below += 52;
        }
        // Legend: the eight buttons that count as a press.
        const int legend_y = std::max(below + 30, 396);
        for (int s = 0; s < vault::kSymbols; ++s) draw_symbol(s, 480 + (s - 3.5f) * 64, legend_y, 13, c.muted, c.background);
        std::string message = notice.empty() ? entry.message : notice;
        if (!message.empty()) {
            int w = rich_text_width(env.font, 0.6f, message.c_str());
            rich_text_draw(env.font, 480 - w / 2, 470, notice.empty() ? entry.message_color : c.warn, 0.6f, message.c_str());
        }
        end_frame();
    }
}

// ---- passphrase entry: the app's own on-screen keyboard ---------------------------------------------
// The system IME learns every word typed into it, even masked, and then suggests the passphrase in
// ordinary text fields. This keyboard is drawn and handled here, so the text never reaches the IME.
struct TouchTap {
    bool was_down = false;
    bool poll(int& x, int& y) {
        SceTouchData data{};
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &data, 1);
        const bool down = data.reportNum > 0;
        const bool tap = down && !was_down;
        if (down) { x = data.report[0].x / 2; y = data.report[0].y / 2; }   // panel 1920x1088 -> screen 960x544
        was_down = down;
        return tap;
    }
};

enum class Act { Char, Shift, Symbols, Language, Space, Delete, Clear, Cancel, Ok };
struct Key { int x, y, w, h; Act act; std::string label; };
struct Layer { const char* name; std::vector<std::vector<std::string>> rows; };

std::vector<std::string> utf8_chars(const char* text) {
    std::vector<std::string> out;
    for (const char* p = text; *p;) {
        size_t n = 1;
        while ((static_cast<unsigned char>(p[n]) & 0xc0) == 0x80) ++n;
        out.emplace_back(p, n);
        p += n;
    }
    return out;
}

const std::vector<Layer>& keyboard_layers() {
    static const std::vector<Layer> layers = [] {
        auto make = [](const char* name, std::initializer_list<const char*> rows) {
            Layer layer{name, {}};
            for (const char* row : rows) layer.rows.push_back(utf8_chars(row));
            return layer;
        };
        return std::vector<Layer>{
            make("abc", {"qwertyuiop", "asdfghjkl", "zxcvbnm"}),
            make("ABC", {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"}),
            make("@123", {"1234567890!@#$", "%^&*()-_=+[]{}", "\\|;:'\",.<>/?`~"}),
            make("\xd0\xb0\xd0\xb1\xd0\xb2", {"\xd0\xb9\xd1\x86\xd1\x83\xd0\xba\xd0\xb5\xd0\xbd\xd0\xb3\xd1\x88\xd1\x89\xd0\xb7\xd1\x85",
                "\xd1\x84\xd1\x8b\xd0\xb2\xd0\xb0\xd0\xbf\xd1\x80\xd0\xbe\xd0\xbb\xd0\xb4\xd0\xb6\xd1\x8d",
                "\xd1\x8f\xd1\x87\xd1\x81\xd0\xbc\xd0\xb8\xd1\x82\xd1\x8c\xd0\xb1\xd1\x8e\xd1\x8a\xd1\x91"}),
            make("\xd0\x90\xd0\x91\xd0\x92", {"\xd0\x99\xd0\xa6\xd0\xa3\xd0\x9a\xd0\x95\xd0\x9d\xd0\x93\xd0\xa8\xd0\xa9\xd0\x97\xd0\xa5",
                "\xd0\xa4\xd0\xab\xd0\x92\xd0\x90\xd0\x9f\xd0\xa0\xd0\x9e\xd0\x9b\xd0\x94\xd0\x96\xd0\xad",
                "\xd0\xaf\xd0\xa7\xd0\xa1\xd0\x9c\xd0\x98\xd0\xa2\xd0\xac\xd0\x91\xd0\xae\xd0\xaa\xd0\x81"}),
        };
    }();
    return layers;
}

size_t count_chars(const std::string& text) {
    size_t n = 0;
    for (unsigned char b : text) if ((b & 0xc0) != 0x80) ++n;
    return n;
}

End read_text(const Env& env, Pad& pad, const Entry& entry, vault::Pin& out) {
    Colors c = colors(env.dark);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    TouchTap touch;
    const auto& layers = keyboard_layers();
    size_t layer = 0;
    bool russian = false, shifted = false, symbols = false, cancel = false;
    int row = 0, col = 0;
    struct Hold : KeyboardHold {
        bool touch = false;
        Key key{};
        std::string digit;
    } hold;
    std::string text;
    struct Wipe { std::string& s; ~Wipe() { std::fill(s.begin(), s.end(), '\0'); s.clear(); } } wipe_on_exit{text};
    // Keep the keyboard anchored like the Vita IME, regardless of hint/error length.
    constexpr int dots_y = 216, panel_top = 250, keys_top = 262, key_h = 64, gap = 8;
    float hint_scale = 0.6f;
    while (hint_scale > 0.42f &&
           (wrap(env.font, entry.hint, 864, hint_scale).size() +
            wrap(env.font, entry.message, 864, hint_scale).size()) * 23 > 112)
        hint_scale -= 0.03f;
    for (;;) {
        layer = symbols ? 2 : (russian ? 3 : 0) + (shifted ? 1 : 0);
        // Geometry is shared by rendering, controller focus and touch hit testing.
        std::vector<std::vector<Key>> grid;
        const Layer& current = layers[layer];
        int y = keys_top;
        for (size_t r = 0; r < current.rows.size(); ++r) {
            const auto& labels = current.rows[r];
            const bool last = r + 1 == current.rows.size();
            const int side = last ? 82 : 0;
            const int available = 928 - (last ? 2 * (side + gap) : 0);
            const int kw = std::min(88, (available - ((int)labels.size() - 1) * gap) / (int)labels.size());
            const int width = (int)labels.size() * (kw + gap) - gap + (last ? 2 * (side + gap) : 0);
            int x = 480 - width / 2;
            std::vector<Key> line;
            if (last) { line.push_back({x, y, side, key_h, Act::Shift, ""}); x += side + gap; }
            for (const auto& label : labels) { line.push_back({x, y, kw, key_h, Act::Char, label}); x += kw + gap; }
            if (last) line.push_back({x, y, side, key_h, Act::Delete, ""});
            grid.push_back(std::move(line));
            y += key_h + gap;
        }
        const int widths[] = {64, 104, 90, 394, 124, 112};
        const Act acts[] = {Act::Cancel, Act::Symbols, Act::Language, Act::Space, Act::Clear, Act::Ok};
        const std::string labels[] = {"", symbols ? (shifted ? "ABC" : "abc") : "@123",
                                     russian ? "EN" : "RU", tr("vault.kb.space"), "", tr("vault.kb.ok")};
        int x = 16;
        std::vector<Key> line;
        for (int i = 0; i < 6; ++i) { line.push_back({x, y, widths[i], key_h, acts[i], labels[i]}); x += widths[i] + gap; }
        grid.push_back(std::move(line));
        row = std::max(0, std::min(row, (int)grid.size() - 1));
        col = std::max(0, std::min(col, (int)grid[row].size() - 1));

        auto press = [&](const Key& key) -> bool {   // true: submit
            switch (key.act) {
                case Act::Char:
                    if (text.size() + key.label.size() <= vault::kMaxText) text += key.label;
                    break;
                case Act::Space: if (text.size() < vault::kMaxText) text += ' '; break;
                case Act::Shift: shifted = !shifted; symbols = false; break;
                case Act::Symbols: symbols = !symbols; break;
                case Act::Language: russian = !russian; symbols = false; break;
                case Act::Cancel: cancel = true; break;
                case Act::Clear: std::fill(text.begin(), text.end(), '\0'); text.clear(); break;
                case Act::Delete:
                    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0x80) text.pop_back();
                    if (!text.empty()) text.pop_back();
                    break;
                case Act::Ok: return !text.empty();
            }
            return false;
        };

        unsigned pressed = pad.poll();
        int tx = 0, ty = 0;
        const bool tapped = touch.poll(tx, ty);
        if (pressed & (SCE_CTRL_SELECT | SCE_CTRL_CIRCLE)) return End::Select;
        bool submit = (pressed & SCE_CTRL_START) && !text.empty();
        const int focus_x = grid[row][col].x + grid[row][col].w / 2;
        if (pressed & SCE_CTRL_UP) row = (row + (int)grid.size() - 1) % (int)grid.size();
        if (pressed & SCE_CTRL_DOWN) row = (row + 1) % (int)grid.size();
        if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
            col = 0;
            for (int k = 1; k < (int)grid[row].size(); ++k)
                if (std::abs(grid[row][k].x + grid[row][k].w / 2 - focus_x) <
                    std::abs(grid[row][col].x + grid[row][col].w / 2 - focus_x)) col = k;
        }
        if (pressed & SCE_CTRL_LEFT) col = (col + (int)grid[row].size() - 1) % (int)grid[row].size();
        if (pressed & SCE_CTRL_RIGHT) col = (col + 1) % (int)grid[row].size();
        auto activate = [&](const Key& key, int r, int k, bool by_touch) {
            hold.active = false;
            if (!symbols && r == 0 && k < 10 && key.act == Act::Char) {
                hold.start(sceKernelGetSystemTimeWide()); hold.touch = by_touch; hold.key = key;
                hold.digit = std::to_string((k + 1) % 10);
            } else submit = press(key) || submit;
        };
        if (hold.active) {
            const bool down = hold.touch ? touch.was_down : (pad.old & SCE_CTRL_CROSS) != 0;
            const bool moved = hold.touch && down &&
                (tx < hold.key.x || tx >= hold.key.x + hold.key.w || ty < hold.key.y || ty >= hold.key.y + hold.key.h);
            const unsigned other = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT |
                SCE_CTRL_TRIANGLE | SCE_CTRL_SQUARE | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
            switch (hold.update(sceKernelGetSystemTimeWide(), down, moved || (pressed & other))) {
                case KeyboardHold::Result::Short: press(hold.key); break;
                case KeyboardHold::Result::Long: press({0, 0, 0, 0, Act::Char, hold.digit}); break;
                case KeyboardHold::Result::None: break;
            }
        }
        if (pressed & SCE_CTRL_CROSS) activate(grid[row][col], row, col, false);
        if (pressed & SCE_CTRL_SQUARE) press({0, 0, 0, 0, Act::Space, ""});
        if (pressed & SCE_CTRL_TRIANGLE) press({0, 0, 0, 0, Act::Symbols, ""});
        if (pressed & SCE_CTRL_LTRIGGER) press({0, 0, 0, 0, Act::Delete, ""});
        if (pressed & SCE_CTRL_RTRIGGER) press({0, 0, 0, 0, Act::Clear, ""});
        if (tapped) {
            for (int r = 0; r < (int)grid.size(); ++r)
                for (int k = 0; k < (int)grid[r].size(); ++k) {
                    const Key& key = grid[r][k];
                    if (tx >= key.x && tx < key.x + key.w && ty >= key.y && ty < key.y + key.h) {
                        row = r; col = k;
                        activate(key, r, k, true);
                    }
                }
        }
        if (cancel) return End::Select;
        if (submit) { out.assign(text.begin(), text.end()); return End::Submit; }

        begin_frame(env, entry.title, "");
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(48, 54, 912, 184);
        int hint_y = draw_lines(env, entry.hint, 48, 72, 864, hint_scale, c.ink, 23);
        if (!entry.message.empty()) draw_lines(env, entry.message, 48, hint_y + 8, 864, hint_scale, entry.message_color, 23);
        vita2d_disable_clipping();
        if (!ui_skin::nine(ui_skin::Slice::MediaFrame, 40, 190, 880, 50))
            vita2d_draw_rectangle(40, 190, 880, 50, c.panel);
        // Draw an opaque full-width sheet over every underlying card/footer.
        // The PNG's transparent corners must never expose the application below it.
        vita2d_draw_rectangle(0, panel_top, 960, 544 - panel_top, c.panel);
        ui_skin::nine(ui_skin::Slice::Panel, 0, panel_top, 960, 544 - panel_top);
        vita2d_draw_rectangle(0, panel_top, 960, 2, c.separator);
        // The phrase is never shown: one dot per character (the last 32), plus the count.
        const size_t chars = count_chars(text);
        const int shown = (int)std::min<size_t>(chars, 32);
        if (shown == 0) { vita2d_draw_fill_circle(480, dots_y, 8, c.separator); vita2d_draw_fill_circle(480, dots_y, 5, c.panel); }
        for (int i = 0; i < shown; ++i) vita2d_draw_fill_circle(480 + (i - (shown - 1) / 2.0f) * 22, dots_y, 8, c.accent);
        const std::string counter = std::to_string(chars);
        rich_text_draw(env.font, 912 - rich_text_width(env.font, 0.58f, counter.c_str()), dots_y + 6, c.muted, 0.58f, counter.c_str());
        for (int r = 0; r < (int)grid.size(); ++r)
            for (int k = 0; k < (int)grid[r].size(); ++k) {
                const Key& key = grid[r][k];
                const bool selected = r == row && k == col;
                const bool ok = key.act == Act::Ok;
                const bool toggled = (key.act == Act::Shift && shifted && !symbols) ||
                                     (key.act == Act::Symbols && symbols);
                const auto slice = selected || toggled ? ui_skin::Slice::RowFocused : ui_skin::Slice::Row;
                const unsigned rim = selected || toggled ? c.accent : c.muted;
                rounded_key_base(key.x, key.y, key.w, key.h, 8, rim);
                rounded_key_base(key.x + 2, key.y + 2, key.w - 4, key.h - 4, 6, c.panel);
                ui_skin::nine(slice, key.x + 2, key.y + 2, key.w - 4, key.h - 4);
                const unsigned color = ok ? c.ok : c.ink;
                if (selected) ui_skin::nine(ui_skin::Slice::FocusRing, key.x - 2, key.y - 2, key.w + 4, key.h + 4);
                const float cx = key.x + key.w / 2.0f;
                const float cy = key.y + key.h / 2.0f +
                    ((key.act == Act::Delete || key.act == Act::Clear || key.act == Act::Cancel) ? 8 : 0);
                if (key.act == Act::Shift) {
                    triangle(cx, cy - 4, 7, 0, toggled ? c.accent : color);
                    vita2d_draw_rectangle(cx - 4, cy + 2, 8, 10, toggled ? c.accent : color);
                } else if (key.act == Act::Delete) {
                    // Backspace outline and cross, independent of font coverage.
                    vita2d_draw_line(cx - 18, cy, cx - 8, cy - 10, color);
                    vita2d_draw_line(cx - 18, cy, cx - 8, cy + 10, color);
                    vita2d_draw_line(cx - 8, cy - 10, cx + 17, cy - 10, color);
                    vita2d_draw_line(cx - 8, cy + 10, cx + 17, cy + 10, color);
                    vita2d_draw_line(cx + 17, cy - 10, cx + 17, cy + 10, color);
                    draw_symbol(4, cx + 4, cy, 6, color, c.panel);
                } else if (key.act == Act::Clear) {
                    // Trash can: clear the whole phrase, paired with the R shortcut.
                    vita2d_draw_line(cx - 10, cy - 8, cx + 10, cy - 8, color);
                    vita2d_draw_line(cx - 6, cy - 12, cx + 6, cy - 12, color);
                    vita2d_draw_line(cx - 8, cy - 6, cx - 6, cy + 12, color);
                    vita2d_draw_line(cx + 8, cy - 6, cx + 6, cy + 12, color);
                    vita2d_draw_line(cx - 6, cy + 12, cx + 6, cy + 12, color);
                    vita2d_draw_line(cx - 3, cy - 3, cx - 3, cy + 8, color);
                    vita2d_draw_line(cx + 3, cy - 3, cx + 3, cy + 8, color);
                } else if (key.act == Act::Cancel) {
                    draw_symbol(5, cx, cy + 4, 10, color, c.panel);
                } else {
                    float scale = key.act == Act::Char ? 0.75f : 0.62f;
                    int w = rich_text_width(env.font, scale, key.label.c_str());
                    if (w > key.w - 12) { scale *= (key.w - 12.0f) / w; w = rich_text_width(env.font, scale, key.label.c_str()); }
                    rich_text_draw(env.font, key.x + (key.w - w) / 2, cy + (key.act == Act::Char ? 8 : 14), color, scale, key.label.c_str());
                }
                // Render shortcut badges directly from the theme's controller atlas.
                int shortcut = -1;
                switch (key.act) {
                    case Act::Symbols: shortcut = 0; break;
                    case Act::Space: shortcut = 1; break;
                    case Act::Delete: shortcut = 7; break;
                    case Act::Clear: shortcut = 8; break;
                    case Act::Cancel: shortcut = 6; break;
                    case Act::Ok: shortcut = 5; break;
                    default: break;
                }
                if (shortcut >= 0) {
                    ui_skin::Sprite sprite;
                    if (ui_skin::button(shortcut, sprite) && sprite.vh > 0) {
                        const float factor = 18.0f / sprite.vh;
                        const float x = key.x + key.w - 6 - sprite.vw * factor;
                        const float y = key.y + 6;
                        vita2d_draw_texture_tint_part_scale(sprite.texture,
                            x - sprite.vx * factor, y - sprite.vy * factor,
                            sprite.x, sprite.y, sprite.w, sprite.h, factor, factor,
                            selected ? RGBA8(255, 255, 255, 255) : RGBA8(220, 230, 240, 255));
                    } else {
                        // Missing assets still show a button name or shape, never a token.
                        const unsigned tint = selected ? c.accent : c.muted;
                        if (shortcut <= 1) draw_symbol(shortcut == 0 ? 6 : 7,
                            key.x + key.w - 15, key.y + 15, 6, tint, c.panel);
                        else {
                            const char* name = shortcut == 7 ? "L" : shortcut == 8 ? "R" : shortcut == 5 ? "START" : "SELECT";
                            const int width = rich_text_width(env.font, 0.38f, name);
                            rich_text_draw(env.font, key.x + key.w - width - 6, key.y + 19, tint, 0.38f, name);
                        }
                    }
                }
                if (!symbols && r == 0 && k < 10) {
                    const std::string digit = std::to_string((k + 1) % 10);
                    const int width = rich_text_width(env.font, 0.42f, digit.c_str());
                    rich_text_draw(env.font, key.x + key.w - width - 7, key.y + 19,
                        hold.active && hold.fired && row == r && col == k ? c.accent : c.muted,
                        0.42f, digit.c_str());
                }
            }
        end_frame();
    }
}

End read_secret(const Env& env, Pad& pad, const Entry& entry, vault::Method method, vault::Pin& pin) {
    return method == vault::Method::Text ? read_text(env, pad, entry, pin) : read_pin(env, pad, entry, pin);
}

std::string entry_footer(bool select_forgot) {
    return tr(select_forgot ? "vault.unlock.footer" : "vault.entry.footer");
}

// ---- screens -----------------------------------------------------------------------------------
bool forgot_screen(const Env& env, Pad& pad) {
    Colors c = colors(env.dark);
    for (;;) {
        unsigned pressed = pad.poll();
        if (pressed & SCE_CTRL_CIRCLE) return false;
        if (pressed & SCE_CTRL_TRIANGLE) return true;
        begin_frame(env, tr("vault.forgot.title"), tr("vault.forgot.footer"));
        draw_lines(env, tr("vault.forgot.body"), 48, 80, 864, 0.62f, c.ink, 28);
        end_frame();
    }
}

void erase_everything(const Env& env) {
    Colors c = colors(env.dark);
    begin_frame(env, tr("vault.forgot.title"), "");
    rich_text_draw(env.font, 48, 250, c.ink, 0.8f, tr("vault.erasing").c_str());
    end_frame();
    account_cleanup::purge(); // TDLib is not running yet at this point
    std::remove("ux0:data/vitagram/settings.json");
    std::remove("ux0:data/vitagram/settings.json.tmp");
    for (const auto& path : vault_sweep::list_user_files()) std::remove(path.c_str());
    vault::reset();
}

// A message the user must acknowledge with {cross}.
void notice_screen(const Env& env, Pad& pad, const std::string& body) {
    Colors c = colors(env.dark);
    for (;;) {
        if (pad.poll() & SCE_CTRL_CROSS) return;
        begin_frame(env, tr("vault.title"), tr("vault.notice.footer"));
        draw_lines(env, body, 48, 80, 864, 0.62f, c.ink, 28);
        end_frame();
        sceKernelDelayThread(1000);
    }
}

// After a cancelled passphrase dialog: type again, or go to the "forgot" path.
bool retry_or_forgot(const Env& env, Pad& pad) {
    Colors c = colors(env.dark);
    for (;;) {
        unsigned pressed = pad.poll();
        if (pressed & SCE_CTRL_CROSS) return false;
        if (pressed & SCE_CTRL_TRIANGLE) return true;
        begin_frame(env, tr("vault.unlock.title"), tr("vault.text.retry_footer"));
        draw_lines(env, tr("vault.text.cancelled"), 48, 80, 864, 0.62f, c.ink, 28);
        end_frame();
    }
}

void unlock_screen(const Env& env, Pad& pad) {
    Colors c = colors(env.dark);
    std::string message;
    unsigned color = c.danger;
    const vault::Method method = vault::method();
    for (;;) {
        Entry entry;
        entry.title = tr("vault.unlock.title");
        entry.hint = vault::disabling() ? tr("vault.unlock.hint_disabling")
                                        : tr(method == vault::Method::Text ? "vault.text.unlock_hint" : "vault.unlock.hint");
        entry.footer = entry_footer(true);
        entry.message = message; entry.message_color = color;
        vault::Pin pin;
        if (read_secret(env, pad, entry, method, pin) == End::Select) {
            bool forgot = method == vault::Method::Text ? retry_or_forgot(env, pad) : true;
            if (forgot && forgot_screen(env, pad)) { erase_everything(env); return; }
            continue;
        }
        if (int wait = vault::lockout_seconds()) {
            message = tr("vault.locked_out", {std::to_string(wait)}); color = c.danger;
            continue;
        }
        Env context = env;
        vault::Result r = vault::unlock(pin, derive_progress, &context);
        for (auto& symbol : pin) symbol = 0;
        if (r == vault::Result::Ok) {
            if (vault::state_was_reset()) notice_screen(env, pad, tr("vault.state_reset"));
            return;
        }
        message = r == vault::Result::WrongPin && vault::lockout_seconds()
                      ? tr("vault.locked_out", {std::to_string(vault::lockout_seconds())}) : result_text(r);
        color = c.danger;
    }
}

enum class Answer { Encrypt, Decline };
Answer ask_screen(const Env& env, Pad& pad) {
    Colors c = colors(env.dark);
    constexpr int top = 60, bottom = 480, step = 28;
    // Reserve a gutter for warning marks and a scrollbar, in every language.
    std::vector<std::string> lines;
    std::vector<bool> warnings;
    const std::string body = tr("vault.ask.body");
    for (size_t start = 0;;) {
        const size_t end = body.find('\n', start);
        std::string paragraph = body.substr(start, end == std::string::npos ? end : end - start);
        const bool warning = paragraph.compare(0, 3, "⚠") == 0;
        if (warning) {
            paragraph.erase(0, 3);
            if (paragraph.compare(0, 3, "\xef\xb8\x8f") == 0) paragraph.erase(0, 3);
            if (!paragraph.empty() && paragraph.front() == ' ') paragraph.erase(0, 1);
        }
        const auto wrapped = wrap(env.font, paragraph, 832, 0.62f);
        for (size_t i = 0; i < wrapped.size(); ++i) {
            lines.push_back(wrapped[i]);
            warnings.push_back(warning && i == 0);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    const int height = (int)lines.size() * step;
    const int max_scroll = std::max(0, height - (bottom - top));
    int scroll = 0, touch_y = 0;
    bool dragging = false;
    unsigned repeat_button = 0;
    int64_t repeat_at = 0;
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    for (;;) {
        unsigned pressed = pad.poll();
        if (pressed & SCE_CTRL_CROSS) return Answer::Encrypt;
        if (pressed & SCE_CTRL_CIRCLE) return Answer::Decline;
        const int64_t now = sceKernelGetSystemTimeWide();
        unsigned direction = pad.old & (SCE_CTRL_UP | SCE_CTRL_DOWN);
        if (direction != repeat_button) {
            repeat_button = direction;
            repeat_at = now + 350000;
        } else if (direction && now >= repeat_at) {
            pressed |= direction;
            repeat_at = now + 70000;
        }
        if (pressed & SCE_CTRL_UP) scroll -= step;
        if (pressed & SCE_CTRL_DOWN) scroll += step;
        SceTouchData touch{};
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        if (touch.reportNum) {
            const int x = touch.report[0].x / 2, y = touch.report[0].y / 2;
            if (dragging) scroll += touch_y - y;
            else dragging = x >= 32 && x < 928 && y >= top && y < bottom;
            touch_y = y;
        } else dragging = false;
        scroll = std::max(0, std::min(max_scroll, scroll));
        begin_frame(env, tr("vault.ask.title"), tr("vault.ask.footer") + "    " + tr("vault.ask.scroll"));
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(32, top, 928, bottom);
        for (size_t i = 0; i < lines.size(); ++i) {
            const int y = top + 20 + (int)i * step - scroll;
            if (y + step < top || y - step > bottom) continue;
            if (warnings[i]) {
                triangle(48, y - 9, 10, 0, c.warn);
                vita2d_draw_rectangle(47, y - 13, 2, 8, c.ink);
                vita2d_draw_rectangle(47, y - 3, 2, 2, c.ink);
            }
            rich_text_draw(env.font, 72, y, c.ink, 0.62f, lines[i].c_str());
        }
        vita2d_disable_clipping();
        if (max_scroll > 0) {
            const float thumb = std::max(24.0f, (float)(bottom - top) * (bottom - top) / height);
            vita2d_draw_rectangle(920, top, 4, bottom - top, c.separator);
            vita2d_draw_rectangle(920, top + (bottom - top - thumb) * scroll / max_scroll, 4, thumb, c.accent);
        }
        end_frame();
        sceKernelDelayThread(1000);
    }
}

enum class Choice { Buttons, Text, Cancel };
Choice choose_method(const Env& env, Pad& pad) {
    Colors c = colors(env.dark);
    for (;;) {
        unsigned pressed = pad.poll();
        if (pressed & SCE_CTRL_CROSS) return Choice::Buttons;
        if (pressed & SCE_CTRL_SQUARE) return Choice::Text;
        if (pressed & SCE_CTRL_CIRCLE) return Choice::Cancel;
        begin_frame(env, tr("vault.method.title"), tr("vault.method.footer"));
        int y = draw_lines(env, tr("vault.method.buttons"), 48, 90, 864, 0.62f, c.ink, 28);
        draw_lines(env, tr("vault.method.text"), 48, y + 30, 864, 0.62f, c.ink, 28);
        end_frame();
    }
}

// Shared by the first-run question and the Settings rows: choose and confirm a new key.
Outcome create_key(const Env& env, Pad& pad, bool changing, const vault::Pin* old_pin) {
    Colors c = colors(env.dark);
    Choice choice = choose_method(env, pad);
    if (choice == Choice::Cancel) return Outcome::Cancelled;
    const vault::Method method = choice == Choice::Text ? vault::Method::Text : vault::Method::Buttons;
    const bool text = method == vault::Method::Text;
    std::string message;
    unsigned color = c.danger;
    for (;;) {
        Entry first_entry;
        first_entry.title = tr(changing ? "vault.new.title" : "vault.setup.title");
        first_entry.hint = text ? tr("vault.text.hint", {std::to_string(vault::kMinText)})
                                : tr("vault.setup.hint", {std::to_string(vault::kMinPin)});
        first_entry.footer = entry_footer(false);
        first_entry.message = message; first_entry.message_color = color;
        first_entry.show_strength = true;
        vault::Pin first;
        if (read_secret(env, pad, first_entry, method, first) == End::Select) return Outcome::Cancelled;
        std::string why;
        if (!vault::secret_acceptable(first, method, &why)) {
            message = why == "short" ? (text ? tr("vault.text.short", {std::to_string(vault::kMinText)})
                                             : tr("vault.weak.short", {std::to_string(vault::kMinPin)}))
                                     : tr("vault.weak.pattern");
            color = c.danger;
            for (auto& b : first) b = 0;
            continue;
        }
        Entry second_entry;
        second_entry.title = tr("vault.setup.confirm_title");
        second_entry.hint = text ? tr("vault.text.confirm_hint", {std::to_string(vault::secret_strength_bits(first, method))})
                                 : tr("vault.setup.confirm_hint");
        second_entry.footer = entry_footer(false);
        vault::Pin second;
        if (read_secret(env, pad, second_entry, method, second) == End::Select) return Outcome::Cancelled;
        if (first != second) { message = tr("vault.mismatch"); color = c.danger; for (auto& b : first) b = 0; for (auto& b : second) b = 0; continue; }
        Env context = env;
        vault::Result r = changing ? vault::change_pin(*old_pin, first, method, derive_progress, &context)
                                   : vault::enable(first, method, derive_progress, &context);
        for (auto& symbol : first) symbol = 0;
        for (auto& symbol : second) symbol = 0;
        if (r == vault::Result::Ok) return Outcome::Done;
        message = result_text(r); color = c.danger;
        if (r == vault::Result::NoMemory || r == vault::Result::Error) return Outcome::Failed;
    }
}

// Asks for the current key until it verifies. `after` runs on success with the verified key.
Outcome ask_current(const Env& env, Pad& pad, const char* title_key, vault::Pin& out) {
    Colors c = colors(env.dark);
    std::string message;
    for (;;) {
        Entry entry;
        entry.title = tr(title_key);
        entry.hint = tr(vault::method() == vault::Method::Text ? "vault.text.current_hint" : "vault.current.hint");
        entry.footer = entry_footer(false);
        entry.message = message; entry.message_color = c.danger;
        if (read_secret(env, pad, entry, vault::method(), out) == End::Select) return Outcome::Cancelled;
        if (int wait = vault::lockout_seconds()) { message = tr("vault.locked_out", {std::to_string(wait)}); continue; }
        Env context = env;
        vault::Result r = vault::verify_pin(out, derive_progress, &context);
        if (r == vault::Result::Ok) return Outcome::Done;
        message = r == vault::Result::WrongPin && vault::lockout_seconds()
                      ? tr("vault.locked_out", {std::to_string(vault::lockout_seconds())}) : result_text(r);
        if (r != vault::Result::WrongPin) return Outcome::Failed;
    }
}

struct Migration {
    const Env* env;
    Pad* pad;
    bool to_sealed;
    size_t done = 0, total = 0;
    int64_t last_draw = 0;
    bool skipped = false;
};

void draw_migration(Migration& m) {
    Colors c = colors(m.env->dark);
    begin_frame(*m.env, tr("vault.title"), "");
    rich_text_draw(m.env->font, 48, 200, c.ink, 0.8f,
                   tr(m.to_sealed ? "vault.migrate.encrypting" : "vault.migrate.decrypting",
                      {std::to_string(m.done), std::to_string(m.total)}).c_str());
    progress_bar(*m.env, 48, 225, 864, m.total ? (float)m.done / m.total : 0.0f, c.accent);
    draw_lines(*m.env, tr("vault.migrate.hint"), 48, 280, 864, 0.58f, c.muted);
    end_frame();
}

bool migration_proceed(void* opaque) {
    auto* m = static_cast<Migration*>(opaque);
    if (m->pad->poll() & SCE_CTRL_CIRCLE) m->skipped = true;
    int64_t now = sceKernelGetSystemTimeWide();
    if (now - m->last_draw > 150000) { m->last_draw = now; draw_migration(*m); }
    return !m->skipped;
}

void migration_report(void* opaque, size_t done, size_t total) {
    auto* m = static_cast<Migration*>(opaque);
    m->done = done; m->total = total;
}
}  // namespace

void run_gate(const Env& env) {
    Pad pad;
    for (;;) {
        switch (vault::state()) {
            case vault::State::Off:
            case vault::State::Unlocked:
                return;
            case vault::State::Locked:
                unlock_screen(env, pad);
                break;
            case vault::State::Undecided:
                if (ask_screen(env, pad) == Answer::Decline) { vault::decline(); return; }
                if (create_key(env, pad, false, nullptr) == Outcome::Done) return;
                break; // cancelled: ask again
        }
    }
}

void show_notice(const Env& env, const std::string& text) {
    Pad pad;
    notice_screen(env, pad, text);
}

void run_migration(const Env& env) {
    if (!vault::unlocked() || !vault::migration_pending()) return;
    Pad pad;
    Migration m{&env, &pad, !vault::disabling()};
    draw_migration(m);
    auto outcome = vault_sweep::convert_all(m.to_sealed, migration_proceed, &m, migration_report, &m);
    // Done only when every file was listed and converted. Otherwise the flag stays and the next start
    // tries again; while switching off, the key is kept (finish_disable waits for the flag), so a
    // file that could not be decrypted now is never left without its key.
    if (outcome.clean()) { vault::set_migration_pending(false); return; }
    if (!outcome.completed) return; // skipped with {circle}: resumes at the next start
    const size_t problems = outcome.failed + outcome.unlisted;
    std::fprintf(stderr, "Vault: %zu files not converted, %zu unreadable entries\n", outcome.failed, outcome.unlisted);
    notice_screen(env, pad, tr("vault.migrate.failed", {std::to_string(problems)}));
}

void relock(const Env& env, void (*tick)(void*), void* context) {
    struct Tick { Tick(void (*f)(void*), void* c) { g_tick = f; g_tick_context = c; }
                  ~Tick() { g_tick = nullptr; g_tick_context = nullptr; } } scoped_tick(tick, context);
    Pad pad;
    Colors c = colors(env.dark);
    const vault::Method method = vault::method();
    std::string message;
    for (;;) {
        Entry entry;
        entry.title = tr("vault.relock.title");
        entry.hint = tr(method == vault::Method::Text ? "vault.text.unlock_hint" : "vault.unlock.hint");
        entry.footer = tr("vault.relock.footer");
        entry.message = message; entry.message_color = c.danger;
        vault::Pin pin;
        if (read_secret(env, pad, entry, method, pin) == End::Select) continue; // nothing to go back to
        if (int wait = vault::lockout_seconds()) {
            for (auto& symbol : pin) symbol = 0;
            message = tr("vault.locked_out", {std::to_string(wait)});
            continue;
        }
        Env derive_context = env;
        vault::Result r = vault::verify_pin(pin, derive_progress, &derive_context);
        for (auto& symbol : pin) symbol = 0;
        if (r == vault::Result::Ok) return;
        message = r == vault::Result::WrongPin && vault::lockout_seconds()
                      ? tr("vault.locked_out", {std::to_string(vault::lockout_seconds())}) : result_text(r);
    }
}

Outcome enable_flow(const Env& env) {
    Pad pad;
    return create_key(env, pad, false, nullptr);
}

Outcome disable_flow(const Env& env) {
    Pad pad;
    vault::Pin pin;
    Outcome o = ask_current(env, pad, "vault.disable.title", pin);
    if (o != Outcome::Done) return o;
    Env context = env;
    vault::Result r = vault::begin_disable(pin, derive_progress, &context);
    for (auto& symbol : pin) symbol = 0;
    return r == vault::Result::Ok ? Outcome::Done : Outcome::Failed;
}

Outcome change_flow(const Env& env) {
    Pad pad;
    vault::Pin old_pin;
    Outcome o = ask_current(env, pad, "vault.current.title", old_pin);
    if (o != Outcome::Done) return o;
    o = create_key(env, pad, true, &old_pin);
    for (auto& symbol : old_pin) symbol = 0;
    return o;
}

namespace {
struct Scan {
    std::vector<std::string> sealed, plain, bad;
    size_t recent = 0;
    bool verified = false;
};

// Shared progress screen; false when the user pressed Circle.
struct Progress {
    const Env* env;
    Pad* pad;
    const char* key;
    int64_t last_draw = 0;
    bool step(size_t done, size_t total) {
        if (pad->poll() & SCE_CTRL_CIRCLE) return false;
        int64_t now = sceKernelGetSystemTimeWide();
        if (now - last_draw > 120000 || done == total) {
            last_draw = now;
            Colors c = colors(env->dark);
            begin_frame(*env, tr("vault.check.title"), "");
            rich_text_draw(env->font, 48, 200, c.ink, 0.8f, tr(key, {std::to_string(done), std::to_string(total)}).c_str());
            progress_bar(*env, 48, 225, 864, total ? (float)done / total : 0.0f, c.accent);
            draw_lines(*env, tr("vault.check.cancel_hint"), 48, 280, 864, 0.58f, c.muted);
            end_frame();
        }
        return true;
    }
};

bool scan_files(const Env& env, Pad& pad, Scan& scan) {
    scan = Scan();
    auto files = vault_sweep::list_user_files();
    Progress progress{&env, &pad, "vault.check.scanning"};
    const time_t now = std::time(nullptr);
    for (size_t i = 0; i < files.size(); ++i) {
        if (!progress.step(i, files.size())) return false;
        if (vault::is_vault_sealed(files[i])) { scan.sealed.push_back(files[i]); continue; }
        struct stat info{};
        // A file changed in the last two minutes may still be downloading: leave it to the background sealer.
        if (stat(files[i].c_str(), &info) == 0 && (info.st_mtime > now - 120)) ++scan.recent;
        else scan.plain.push_back(files[i]);
    }
    return true;
}

bool verify_files(const Env& env, Pad& pad, Scan& scan) {
    Progress progress{&env, &pad, "vault.check.verifying"};
    scan.bad.clear();
    std::vector<uint8_t> block(65536);
    for (size_t i = 0; i < scan.sealed.size(); ++i) {
        if (!progress.step(i, scan.sealed.size())) return false;
        vault::Reader reader;
        bool ok = reader.open(scan.sealed[i]);
        while (ok) {
            int64_t n = reader.read(block.data(), block.size());
            if (n < 0) ok = false;
            else if (n == 0) break;
            if (!progress.step(i, scan.sealed.size())) return false; // Circle also stops a long file
        }
        if (!ok) scan.bad.push_back(scan.sealed[i]);
    }
    scan.verified = true;
    return true;
}
}  // namespace

Outcome check_flow(const Env& env) {
    Pad pad;
    Scan scan;
    bool scanned = false;
    std::string note;
    Colors c = colors(env.dark);
    for (;;) {
        if (!scanned) {
            if (!scan_files(env, pad, scan)) return Outcome::Cancelled;
            scanned = true;
        }
        std::string footer;
        auto add = [&](const char* key) { if (!footer.empty()) footer += "    "; footer += tr(key); };
        if (!scan.plain.empty()) add("vault.check.f_encrypt");
        if (!scan.sealed.empty()) add("vault.check.f_verify");
        if (!scan.bad.empty()) add("vault.check.f_delete");
        add("vault.check.f_back");
        for (;;) {
            unsigned pressed = pad.poll();
            if (pressed & SCE_CTRL_CIRCLE) return Outcome::Done;
            if ((pressed & SCE_CTRL_CROSS) && !scan.plain.empty()) {
                Progress progress{&env, &pad, "vault.check.encrypting"};
                size_t done = 0, failed = 0;
                for (const auto& path : scan.plain) {
                    if (!progress.step(done, scan.plain.size())) break;
                    auto r = vault::seal_file(path);
                    if (r == vault::Convert::Failed) ++failed;
                    ++done;
                }
                note = tr("vault.check.done", {std::to_string(done - failed), std::to_string(failed)});
                scanned = false;
                break;
            }
            if ((pressed & SCE_CTRL_TRIANGLE) && !scan.sealed.empty()) {
                if (verify_files(env, pad, scan)) note = scan.bad.empty() ? tr("vault.check.verify_ok") : tr("vault.check.verify_bad", {std::to_string(scan.bad.size())});
                break;
            }
            if ((pressed & SCE_CTRL_SQUARE) && !scan.bad.empty()) {
                for (const auto& path : scan.bad) std::remove(path.c_str());
                note = tr("vault.check.deleted", {std::to_string(scan.bad.size())});
                scanned = false;
                break;
            }
            begin_frame(env, tr("vault.check.title"), footer);
            std::string summary = tr("vault.check.summary", {std::to_string(scan.sealed.size() + scan.plain.size() + scan.recent),
                std::to_string(scan.sealed.size()), std::to_string(scan.plain.size()), std::to_string(scan.recent),
                scan.verified ? std::to_string(scan.bad.size()) : std::string("-")});
            int y = draw_lines(env, summary, 48, 80, 864, 0.62f, c.ink, 30);
            if (scan.plain.empty() && scan.recent == 0) draw_lines(env, tr("vault.check.all_ok"), 48, y + 10, 864, 0.62f, c.ok);
            if (!note.empty()) draw_lines(env, note, 48, y + 60, 864, 0.62f, scan.bad.empty() ? c.ok : c.danger);
            end_frame();
        }
    }
}
}
