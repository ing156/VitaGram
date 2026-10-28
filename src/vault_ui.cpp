#include "vault_ui.h"
#include "custom_keyboard.h"
#include "scroll_repeat.h"
#include "account_cleanup.h"
#include "i18n.h"
#include "media_render.h"
#include "vault.h"
#include "vault_sweep.h"
#include "process_modules.h"
#include "config.h"
#include "ui_diagnostics.h"
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
    c.ink = ui_skin::color("text", dark ? RGBA8(231, 238, 245, 255) : RGBA8(39, 51, 66, 255));
    c.muted = ui_skin::color("muted", dark ? RGBA8(163, 183, 200, 255) : RGBA8(105, 122, 140, 255));
    c.accent = ui_skin::color("accent", RGBA8(35, 135, 210, 255));
    c.danger = ui_skin::color("danger", RGBA8(214, 69, 65, 255));
    c.ok = ui_skin::color("green", RGBA8(20, 143, 82, 255));
    c.warn = ui_skin::color("warning", RGBA8(224, 146, 36, 255));
    c.separator = ui_skin::color("divider", dark ? RGBA8(53, 70, 86, 255) : RGBA8(205, 216, 226, 255));
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
    unsigned char left_y = 128;
    Pad() { SceCtrlData d{}; sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG); sceCtrlPeekBufferPositive(0, &d, 1); old = d.buttons; left_y = d.ly; }
    unsigned poll() {
        SceCtrlData d{};
        sceCtrlPeekBufferPositive(0, &d, 1);
        unsigned pressed = d.buttons & ~old;
        old = d.buttons;
        left_y = d.ly;
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

// Installs Env::tick for one settings flow; a tick installed by an enclosing screen is kept.
struct FlowTick {
    bool installed = false;
    explicit FlowTick(const Env& env) {
        if (g_tick || !env.tick) return;
        g_tick = env.tick; g_tick_context = env.tick_context; installed = true;
    }
    ~FlowTick() { if (installed) { g_tick = nullptr; g_tick_context = nullptr; } }
    FlowTick(const FlowTick&) = delete;
    FlowTick& operator=(const FlowTick&) = delete;
};
using ui_diagnostics::Phase;
using ui_diagnostics::Stage;

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
    std::string warning; // foreign plugins in this process (filled by read_secret)
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
        if (!entry.warning.empty()) y = draw_lines(env, entry.warning, 48, y + 6, 864, 0.55f, c.danger);
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

// Passphrases and ordinary text share the same keyboard; secrets never reach the IME.
// Counts in place: a substr() copy of the passphrase would stay behind in freed memory.
size_t count_chars(const std::string& text, size_t bytes = std::string::npos) {
    size_t n = 0;
    const size_t end = std::min(bytes, text.size());
    for (size_t i = 0; i < end; ++i) if (((unsigned char)text[i] & 0xc0) != 0x80) ++n;
    return n;
}
End read_text(const Env& env, Pad& pad, const Entry& entry, vault::Pin& out) {
    Colors c = colors(env.dark);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    CustomKeyboard keyboard;
    keyboard.setLanguages(env.keyboard_languages);
    keyboard.setSplitLevel(env.keyboard_split);
    keyboard.open("", true, false, vault::kMaxText);
    constexpr int dots_y = 216;
    const std::string message = entry.warning.empty() ? entry.message
                              : entry.message.empty() ? entry.warning : entry.message + " " + entry.warning;
    const unsigned message_color = entry.message.empty() ? c.danger : entry.message_color;
    float hint_scale = 0.6f;
    while (hint_scale > 0.42f &&
           (wrap(env.font, entry.hint, 864, hint_scale).size() +
            wrap(env.font, message, 864, hint_scale).size()) * 23 > 112) hint_scale -= 0.03f;
    for (;;) {
        const unsigned pressed = pad.poll();
        SceTouchData touch{}; sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        const auto result = keyboard.handle(pressed, pad.old, touch.reportNum != 0,
            touch.reportNum ? touch.report[0].x / 2 : 0, touch.reportNum ? touch.report[0].y / 2 : 0,
            sceKernelGetSystemTimeWide(), &touch);
        if (result == CustomKeyboard::Result::Cancelled) return End::Select;
        if (result == CustomKeyboard::Result::Submitted) {
            out.assign(keyboard.text().begin(), keyboard.text().end()); return End::Submit;
        }
        begin_frame(env, entry.title, "");
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(48, 54, 912, 184);
        int hint_y = draw_lines(env, entry.hint, 48, 72, 864, hint_scale, c.ink, 23);
        if (!message.empty()) draw_lines(env, message, 48, hint_y + 8, 864, hint_scale, message_color, 23);
        vita2d_disable_clipping();
        if (!ui_skin::nine(ui_skin::Slice::MediaFrame, 40, 190, 880, 50))
            vita2d_draw_rectangle(40, 190, 880, 50, c.panel);
        // The phrase is never shown; keep up to 32 masked characters around the cursor.
        const size_t chars = count_chars(keyboard.text());
        const size_t before = count_chars(keyboard.text(), keyboard.cursor());
        const size_t first = before > 32 ? before - 32 : 0;
        const int shown = (int)std::min<size_t>(chars - first, 32);
        if (shown == 0) { vita2d_draw_fill_circle(480, dots_y, 8, c.separator); vita2d_draw_fill_circle(480, dots_y, 5, c.panel); }
        for (int i = 0; i < shown; ++i) vita2d_draw_fill_circle(480 + (i - (shown - 1) / 2.0f) * 22, dots_y, 8, c.accent);
        vita2d_draw_rectangle(480 - shown*11 + (before-first)*22, dots_y - 11, 2, 22, c.accent);
        const std::string counter = std::to_string(chars);
        rich_text_draw(env.font, 912 - rich_text_width(env.font, 0.58f, counter.c_str()), dots_y + 6, c.muted, 0.58f, counter.c_str());
        keyboard.draw(env.font, env.dark, entry.title, true);
        end_frame();
    }
}

End read_secret(const Env& env, Pad& pad, const Entry& entry, vault::Method method, vault::Pin& pin) {
    // Code injected into this process can watch the key as it is entered: every key screen says so.
    Entry shown = entry;
    const auto plugins = process_modules::foreign(process_modules::kTitleId);
    if (!plugins.empty()) shown.warning = tr("vault.plugins_warning", {process_modules::summary(plugins)});
    return method == vault::Method::Text ? read_text(env, pad, shown, pin) : read_pin(env, pad, shown, pin);
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
        vault::Result r;
        { Stage derive(Phase::VaultDerive); r = vault::unlock(pin, derive_progress, &context); }
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
    ScrollRepeat scroll_repeat;
    ScrollInput scroll_input;
    scroll_input.sample(pad.old & SCE_CTRL_UP, pad.old & SCE_CTRL_DOWN, pad.left_y);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    for (;;) {
        unsigned pressed = pad.poll();
        if (pressed & SCE_CTRL_CROSS) return Answer::Encrypt;
        if (pressed & SCE_CTRL_CIRCLE) return Answer::Decline;
        const int64_t now = sceKernelGetSystemTimeWide();
        scroll_repeat.beginFrame();
        const unsigned held = pad.old & (SCE_CTRL_UP | SCE_CTRL_DOWN);
        const auto direction = scroll_input.sample(held & SCE_CTRL_UP, held & SCE_CTRL_DOWN, pad.left_y);
        if (held == (SCE_CTRL_UP | SCE_CTRL_DOWN)) pressed &= ~held;
        const int repeated = scroll_repeat.update(direction.direction, direction.fresh, 1,
                                                   (double)(bottom - top) / step, now, env.scroll_speed);
        const int navigation = direction.fresh ? direction.direction : repeated;
        if (navigation) pressed |= navigation < 0 ? SCE_CTRL_UP : SCE_CTRL_DOWN;
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
        Stage derive(Phase::VaultDerive);
        vault::Result r = changing ? vault::change_pin(*old_pin, first, method, derive_progress, &context)
                                   : vault::enable(first, method, derive_progress, &context);
        derive.to(Phase::VaultScreen);
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
        vault::Result r;
        { Stage derive(Phase::VaultDerive); r = vault::verify_pin(out, derive_progress, &context); }
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
    Stage stage(Phase::VaultScreen);
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
    Stage stage(Phase::VaultMigration);
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
    Stage stage(Phase::VaultScreen);
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
        vault::Result r;
        { Stage derive(Phase::VaultDerive); r = vault::verify_pin(pin, derive_progress, &derive_context); }
        for (auto& symbol : pin) symbol = 0;
        if (r == vault::Result::Ok) return;
        message = r == vault::Result::WrongPin && vault::lockout_seconds()
                      ? tr("vault.locked_out", {std::to_string(vault::lockout_seconds())}) : result_text(r);
    }
}

Outcome enable_flow(const Env& env) {
    FlowTick tick(env);
    Stage stage(Phase::VaultScreen);
    Pad pad;
    return create_key(env, pad, false, nullptr);
}

Outcome disable_flow(const Env& env) {
    FlowTick tick(env);
    Stage stage(Phase::VaultScreen);
    Pad pad;
    vault::Pin pin;
    Outcome o = ask_current(env, pad, "vault.disable.title", pin);
    if (o != Outcome::Done) return o;
    Env context = env;
    vault::Result r;
    { Stage derive(Phase::VaultDerive); r = vault::begin_disable(pin, derive_progress, &context); }
    for (auto& symbol : pin) symbol = 0;
    return r == vault::Result::Ok ? Outcome::Done : Outcome::Failed;
}

Outcome change_flow(const Env& env) {
    FlowTick tick(env);
    Stage stage(Phase::VaultScreen);
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

// Partial downloads and pending uploads of TDLib: it keeps writing into them, so they stay plain.
bool in_td_temp(const std::string& path) {
    static const std::string temp = std::string(VITAGRAM_FILES_DIR) + "/temp/";
    return !path.compare(0, temp.size(), temp);
}

bool scan_files(const Env& env, Pad& pad, Scan& scan) {
    Stage stage(Phase::VaultScan);
    scan = Scan();
    auto files = vault_sweep::list_user_files();
    Progress progress{&env, &pad, "vault.check.scanning"};
    const time_t now = std::time(nullptr);
    for (size_t i = 0; i < files.size(); ++i) {
        if (!progress.step(i, files.size())) return false;
        if (vault::is_vault_sealed(files[i])) { scan.sealed.push_back(files[i]); continue; }
        if (in_td_temp(files[i])) { ++scan.recent; continue; }
        struct stat info{};
        // A file changed in the last two minutes may still be downloading: leave it to the background sealer.
        if (stat(files[i].c_str(), &info) == 0 && (info.st_mtime > now - 120)) ++scan.recent;
        else scan.plain.push_back(files[i]);
    }
    return true;
}

bool verify_files(const Env& env, Pad& pad, Scan& scan) {
    Stage stage(Phase::VaultVerify);
    vault_sweep::SealerPause hold; // a background rename must not replace a file being read
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
    FlowTick tick(env);
    Stage stage(Phase::VaultScreen);
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
                Stage sealing(Phase::VaultSeal);
                vault_sweep::SealerPause hold; // the background sealer would share "<file>.vgs"
                Progress progress{&env, &pad, "vault.check.encrypting"};
                size_t done = 0, failed = 0;
                // Called every 64 KiB: a large video keeps the screen, the event pump and Circle alive.
                struct Step { Progress* progress; size_t done, total; } step{&progress, 0, scan.plain.size()};
                auto proceed = [](void* opaque) -> bool {
                    auto* s = static_cast<Step*>(opaque);
                    return s->progress->step(s->done, s->total);
                };
                for (const auto& path : scan.plain) {
                    if (!progress.step(done, scan.plain.size())) break;
                    step.done = done;
                    auto r = vault::seal_file(path, proceed, &step);
                    if (r == vault::Convert::Interrupted) break;
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
