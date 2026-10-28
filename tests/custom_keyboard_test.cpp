#include "custom_keyboard.h"
#include "keyboard.h"
#include "keyboard_hold.h"
#include "keyboard_case.h"
#include "keyboard_layout.h"
#include "keyboard_languages.h"
#include "media_render.h"
#include "ui_skin.h"
#include "ui_color.h"
#include <map>
#include <array>
#include <cmath>
#include "i18n.h"
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/clib.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include "touch_history.h"

static std::vector<SceTouchData> touch_backlog;
static unsigned touch_reads=0;
int sceTouchRead(SceUInt32 port,SceTouchData* data,SceUInt32 count) {
    assert(port==SCE_TOUCH_PORT_FRONT);
    ++touch_reads;
    const auto copied=std::min<size_t>(count,touch_backlog.size());
    std::copy_n(touch_backlog.begin(),copied,data);
    touch_backlog.erase(touch_backlog.begin(),touch_backlog.begin()+copied);
    return copied;
}

static int ime_inits = 0, ime_terms = 0;
static bool ime_finished = false;
static unsigned short* ime_buffer = nullptr;
static SceImeDialogParam last_ime{};
void* sceClibMemset(void* dst, int ch, SceSize len) { return std::memset(dst, ch, len); }
void vita2d_wait_rendering_done() {}
int sceGxmDisplayQueueFinish() { return 0; }
SceInt32 sceImeDialogInit(const SceImeDialogParam* param) {
    ++ime_inits; last_ime = *param; ime_buffer = param->inputTextBuffer; return 0;
}
SceCommonDialogStatus sceImeDialogGetStatus() {
    return ime_finished ? SCE_COMMON_DIALOG_STATUS_FINISHED : SCE_COMMON_DIALOG_STATUS_RUNNING;
}
SceInt32 sceImeDialogGetResult(SceImeDialogResult* result) { result->button = SCE_IME_DIALOG_BUTTON_ENTER; return 0; }
SceInt32 sceImeDialogTerm() { ++ime_terms; return 0; }
static std::vector<std::string> drawn, previews, language_options;
struct DrawnKey { int x, baseline; std::string label; };
static std::vector<DrawnKey> letter_keys;
struct RenderedText { int x, baseline; std::string label; unsigned color; };
static std::vector<RenderedText> rendered_text;
static std::vector<int> shortcut_badges;
static std::vector<std::pair<int, int>> carets;
static std::map<std::string,unsigned> palette;
static int background_calls=0, header_calls=0, key_slices=0;
static std::vector<unsigned> fills;
static int smooth_contours=0;
static bool record_shapes=false;
struct Shape { float x,y,w,h; unsigned color; };
static std::vector<Shape> shapes;
void* vita2d_pool_memalign(size_t bytes,size_t) {
    static std::array<vita2d_color_vertex,208> pool;
    assert(bytes<=pool.size()*sizeof(vita2d_color_vertex));return pool.data();
}
void vita2d_draw_array(SceGxmPrimitiveType primitive,const vita2d_color_vertex* vertices,size_t count) {
    for(size_t i=0;i<count;++i)assert(std::isfinite(vertices[i].x) && std::isfinite(vertices[i].y));
    if(primitive==SCE_GXM_PRIMITIVE_TRIANGLE_STRIP) {
        assert(count==138);
        for(size_t i=0;i<count;i+=2) {
            const auto a=vertices[i],b=vertices[i+1];
            assert((b.color>>24)==0 && (a.color & 0x00FFFFFFu)==b.color);
            assert(std::fabs((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)-1)<.001f);
        }
        ++smooth_contours;
    } else {
        assert(primitive==SCE_GXM_PRIMITIVE_TRIANGLE_FAN && count==70);
        fills.push_back(vertices[0].color);
        if(record_shapes) {
            float x=vertices[1].x,y=vertices[1].y,right=x,bottom=y;
            for(size_t i=1;i<count;++i){x=std::min(x,vertices[i].x);y=std::min(y,vertices[i].y);right=std::max(right,vertices[i].x);bottom=std::max(bottom,vertices[i].y);}
            shapes.push_back({x,y,right-x,bottom-y,vertices[0].color});
        }
    }
}
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned color) {
    fills.push_back(color);
    if (w == 2 && h == 22) carets.emplace_back(int(x), int(y));
}
void vita2d_draw_fill_circle(float, float, float, unsigned) {}
void vita2d_draw_line(float, float, float, float, unsigned) {}
void vita2d_draw_texture_tint_part_scale(const vita2d_texture*, float, float, float, float, float, float, float, float, unsigned) {}
static size_t width_calls=0;
static unsigned font_revision=0;
unsigned rich_text_revision() { return font_revision; }
int rich_text_width(vita2d_pgf*, float, const char* text) { ++width_calls; return std::strlen(text) * 10; }
void rich_text_draw(vita2d_pgf*, int x, int baseline, unsigned color, float scale, const char* text) {
    drawn.emplace_back(text);
    rendered_text.push_back({x, baseline, text, color});
    if (scale >= 1.0f) previews.emplace_back(text);
    if (scale == 0.68f) language_options.emplace_back(text);
    if (scale == 0.82f) letter_keys.push_back({x + int(std::strlen(text))*5, baseline, text});
}
namespace ui_skin {
bool ready() { return false; }
unsigned surface_color(unsigned fallback) { return fallback; }
unsigned color(const char* role, unsigned fallback) { auto it=palette.find(role); return it==palette.end()?fallback:it->second; }
bool is_dark(bool fallback) { return palette.empty()?fallback:ui_color::luminance(color("background",0))<ui_color::luminance(color("text",0)); }
bool header(float) { ++header_calls; return !palette.empty(); }
bool background(float, float) { ++background_calls; return !palette.empty(); }
bool nine(Slice, float, float, float, float, unsigned) { ++key_slices; return !palette.empty(); }
bool button(int index, Sprite&) { shortcut_badges.push_back(index); return false; }
}

int main(int argc, char** argv) {
    assert(argc == 2);
    i18n::init(argv[1], "en");
    using Result = CustomKeyboard::Result;
    CustomKeyboard kb;
    kb.setLanguages(keyboard_languages::supported());
    auto open = [&](const std::string& text, bool secret, bool message, size_t limit) {
        kb.open(text, secret, message, limit, false, false); assert(kb.setLanguage("en"));
    };
    uint64_t now = 100;
    auto event = [&](unsigned pressed = 0, unsigned held = 0, bool touch = false, int x = 0, int y = 0) {
        now += 1000;
        return kb.handle(pressed, held, touch, x, y, now);
    };
    auto button = [&](unsigned b) { auto r = event(b, b); event(); return r; };
    auto tap = [&](int x, int y) { auto r = event(0, 0, true, x, y); event(); return r; };
    open("", false, true, 1023);
    button(SCE_CTRL_CROSS); assert(kb.text() == "q"); // short release
    event(SCE_CTRL_CROSS, SCE_CTRL_CROSS);
    now += KeyboardHold::delay;
    event(0, SCE_CTRL_CROSS); event(); assert(kb.text() == "q1"); // one alternate digit
    assert(kb.setLanguage("ru")); // RU
    tap(48, 285); assert(kb.text() == "q1й");
    button(SCE_CTRL_SQUARE); assert(kb.text() == "q1"); // whole UTF-8 glyph
    tap(135, 417); // first letter on row 3 (я)
    assert(kb.text() == "q1я");
    // Shift, then the same letter.
    tap(48, 417); tap(135, 417);
    assert(kb.text().find("Я") != std::string::npos);
    const std::string before_clear = kb.text();
    button(SCE_CTRL_RTRIGGER); assert(kb.text() == before_clear);
    open("", false, false, 1023);
    tap(130, 505); tap(45, 285); assert(kb.text() == "1");
    open("", false, false, 1023);
    button(SCE_CTRL_RIGHT); button(SCE_CTRL_CROSS); assert(kb.text() == "w");
    open("one  two", false, true, 1023);
    tap(770, 505); // newline, after the enlarged space key
    button(SCE_CTRL_TRIANGLE); assert(kb.text() == "one  two\n ");
    drawn.clear(); kb.draw(nullptr, true, "compose");
    assert(std::find(drawn.begin(), drawn.end(), "one  two") != drawn.end());
    assert(tap(890, 505) == Result::Submitted);
    assert(button(SCE_CTRL_CIRCLE) == Result::Cancelled);
    open("пароль", true, false, 128);
    drawn.clear(); kb.draw(nullptr, false, "unlock");
    for (const auto& text : drawn) assert(text.find("пароль") == std::string::npos);
    assert(button(SCE_CTRL_START) == Result::Submitted);
    tap(750, 505); assert(kb.text().empty()); // Vault-only touch clear remains available.
    open("", true, false, 128);
    assert(button(SCE_CTRL_START) == Result::None);
    assert(tap(890, 505) == Result::None);
    assert(button(SCE_CTRL_SELECT) == Result::Cancelled);
    // Byte limits never split UTF-8, including non-BMP initial text.
    open("я😀x", false, false, 5); assert(kb.text() == "я");
    open("я😀", false, false, 6); assert(kb.text() == "я😀");
    button(SCE_CTRL_CROSS); assert(kb.text() == "я😀");
    button(SCE_CTRL_SQUARE); assert(kb.text() == "я");
    kb.clear(); assert(kb.text().empty());
    // Empty messages stay open; ordinary search fields can submit empty text.
    open("", false, true, 1023); assert(button(SCE_CTRL_START) == Result::None);
    open("", false, false, 1023); assert(button(SCE_CTRL_START) == Result::Submitted);
    // The wrapper routes custom input without creating or terminating a system dialog.
    Keyboard dialog;
    dialog.configure(true, true); dialog.open("compose", false, "initial", true);
    assert(dialog.active() && dialog.ownsScreen() && !dialog.systemActive() && ime_inits == 0);
    dialog.handle(SCE_CTRL_START, SCE_CTRL_START, false, 0, 0, now++); // opening gesture
    assert(dialog.active() && !dialog.submitted());
    dialog.handle(0, 0, false, 0, 0, now++); // release before accepting input
    dialog.handle(SCE_CTRL_START, SCE_CTRL_START, false, 0, 0, now++);
    assert(!dialog.active() && dialog.submitted() && dialog.text() == "initial");
    dialog.close(); assert(dialog.text().empty() && !dialog.submitted() && ime_terms == 0);
    dialog.configure(false, false); dialog.open("system", false, "draft", true);
    assert(dialog.systemActive() && !dialog.ownsScreen() && ime_inits == 1);
    assert(!(last_ime.option & SCE_IME_OPTION_MULTILINE));
    ime_buffer[0] = 0x044f; ime_buffer[1] = 0xd83d; ime_buffer[2] = 0xde00; ime_buffer[3] = 0;
    ime_finished = true;
    dialog.handle(0, 0, false, 0, 0, now++);
    assert(dialog.submitted() && dialog.text() == "я😀" && ime_terms == 1);
    dialog.close(); assert(ime_terms == 1 && dialog.text().empty());
    dialog.configure(true, true); dialog.open("cancel");
    dialog.handle(0, 0, false, 0, 0, now++);
    dialog.handle(SCE_CTRL_CIRCLE, SCE_CTRL_CIRCLE, false, 0, 0, now++);
    assert(dialog.cancelled() && !dialog.active()); dialog.close(); assert(ime_terms == 1);
    // Character preview follows a hold from letter to digit, then disappears on release.
    open("", false, true, 1023);
    event(SCE_CTRL_CROSS, SCE_CTRL_CROSS);
    previews.clear(); kb.draw(nullptr, true, "compose");
    assert(previews == std::vector<std::string>{"q"});
    now += KeyboardHold::delay;
    event(0, SCE_CTRL_CROSS);
    previews.clear(); kb.draw(nullptr, true, "compose");
    assert(previews == std::vector<std::string>{"1"});
    event(); previews.clear(); kb.draw(nullptr, true, "compose"); assert(previews.empty());
    open("", true, false, 128);
    event(SCE_CTRL_CROSS, SCE_CTRL_CROSS);
    previews.clear(); kb.draw(nullptr, true, "unlock"); assert(previews.empty());
    event();
    // The former clear key area now inserts a newline for messages, and space in other fields.
    open("keep", false, true, 1023);
    tap(770, 505); assert(kb.text() == "keep\n");
    button(SCE_CTRL_RTRIGGER); assert(kb.text() == "keep\n");
    open("keep", false, false, 1023);
    tap(700, 505); assert(kb.text() == "keep ");
    button(SCE_CTRL_RTRIGGER); assert(kb.text() == "keep ");
    // Standard RU rows: rare letters are alternates, not separate cramped keys.
    open("", false, true, 1023); assert(kb.setLanguage("ru"));
    letter_keys.clear(); drawn.clear(); kb.draw(nullptr, true, "compose");
    for (auto row : {std::pair<int, int>{303, 11}, {375, 11}, {447, 9}})
        assert(std::count_if(letter_keys.begin(), letter_keys.end(), [&](const DrawnKey& key) {
            return key.baseline == row.first;
        }) == row.second);
    for (const auto& key : letter_keys) assert(key.label != "ё" && key.label != "ъ");
    assert(std::find(drawn.begin(), drawn.end(), "ё 4") != drawn.end()); // Visible hint.
    assert(std::find(drawn.begin(), drawn.end(), "ъ") != drawn.end());
    tap(395, 285); assert(kb.text() == "е");
    tap(648, 417); assert(kb.text() == "еь");
    event(0, 0, true, 395, 285); now += KeyboardHold::delay;
    event(0, 0, true, 395, 285);
    previews.clear(); kb.draw(nullptr, true, "compose"); assert((previews == std::vector<std::string>{"ё", "4"}));
    event(); assert(kb.text() == "еьё");
    event(0, 0, true, 648, 417); now += KeyboardHold::delay;
    event(); assert(kb.text() == "еьёъ"); // Delayed release resolves exactly once.
    tap(48, 417); // Shift.
    event(0, 0, true, 395, 285); now += KeyboardHold::delay;
    event();
    tap(48, 417); // A fresh one-shot Shift for Ъ.
    event(0, 0, true, 648, 417); now += KeyboardHold::delay;
    event(); assert(kb.text() == "еьёъЁЪ");
    // Controller holds use the same alternatives and do not append the base letter.
    tap(648, 417); button(SCE_CTRL_SQUARE); // Focus Ь without retaining its short input.
    event(SCE_CTRL_CROSS, SCE_CTRL_CROSS); now += KeyboardHold::delay;
    event(0, SCE_CTRL_CROSS); event(); assert(kb.text() == "еьёъЁЪъ");
    // Switching layout or dragging away cancels a pending alternate.
    const auto before_alternate = kb.text();
    event(0, 0, true, 395, 285); now += KeyboardHold::delay;
    event(0, 0, true, 900, 200); event(); assert(kb.text() == before_alternate);
    event(0, 0, true, 395, 285); button(SCE_CTRL_TRIANGLE);
    assert(kb.text() == before_alternate);
    // Comma and period need no layout switch; R still cannot erase text.
    open("", false, true, 1023);
    tap(290, 505); tap(710, 505); assert(kb.text() == ",.");
    button(SCE_CTRL_RTRIGGER); assert(kb.text() == ",.");
    // All digits remain available on symbols as well.
    tap(130, 505); tap(310, 285); assert(kb.text() == ",.5");
    // Alternates respect byte limits and remain masked in password entry.
    open("", true, false, 2); assert(kb.setLanguage("ru"));
    event(0, 0, true, 395, 285); now += KeyboardHold::delay;
    event(0, 0, true, 395, 285);
    previews.clear(); kb.draw(nullptr, true, "unlock"); assert(previews.empty());
    event(); assert(kb.text() == "ё");
    tap(648, 417); assert(kb.text() == "ё");
    // Dragging off a held key cancels it, including after the long-press threshold.
    open("", false, false, 1023);
    event(0, 0, true, 48, 285); now += KeyboardHold::delay;
    event(0, 0, true, 900, 200); event(); assert(kb.text().empty());
    // 299.999 ms stays a letter; exactly 300 ms opens choices without inserting.
    open("", false, true, 1023); assert(kb.setLanguage("ru"));
    tap(395, 285); button(SCE_CTRL_SQUARE); // Focus е, preserving an empty editor.
    auto at = [&](uint64_t t, unsigned pressed, unsigned held, bool down = false, int x = 0, int y = 0) {
        now = t; return kb.handle(pressed, held, down, x, y, now);
    };
    uint64_t started = now + 1000000;
    at(started, SCE_CTRL_CROSS, SCE_CTRL_CROSS);
    at(started + 299999, 0, SCE_CTRL_CROSS);
    previews.clear(); kb.draw(nullptr, true, "compose"); assert(previews == std::vector<std::string>{"е"});
    assert(kb.text().empty());
    at(started + 300000, 0, SCE_CTRL_CROSS);
    previews.clear(); kb.draw(nullptr, true, "compose");
    assert((previews == std::vector<std::string>{"ё", "4"})); assert(kb.text().empty());
    at(started + 310000, SCE_CTRL_RIGHT, SCE_CTRL_CROSS | SCE_CTRL_RIGHT);
    assert(kb.text().empty());
    at(started + 320000, 0, 0); assert(kb.text() == "4");
    event(); assert(kb.text() == "4"); // No repeated commit or base letter.
    // The default alternative remains ё. Left/Right cycle while × stays down.
    event(SCE_CTRL_CROSS, SCE_CTRL_CROSS); now += KeyboardHold::delay;
    event(0, SCE_CTRL_CROSS); event(SCE_CTRL_LEFT, SCE_CTRL_CROSS | SCE_CTRL_LEFT);
    event(SCE_CTRL_RIGHT, SCE_CTRL_CROSS | SCE_CTRL_RIGHT); event(); assert(kb.text() == "4ё");
    // Circle closes only the choice panel, and Start cannot send a pending choice.
    event(SCE_CTRL_CROSS, SCE_CTRL_CROSS); now += KeyboardHold::delay;
    event(0, SCE_CTRL_CROSS);
    assert(event(SCE_CTRL_CIRCLE, SCE_CTRL_CROSS | SCE_CTRL_CIRCLE) == Result::None);
    event(); assert(kb.text() == "4ё");
    event(SCE_CTRL_CROSS, SCE_CTRL_CROSS); now += KeyboardHold::delay;
    event(0, SCE_CTRL_CROSS);
    assert(event(SCE_CTRL_START, SCE_CTRL_CROSS | SCE_CTRL_START) == Result::None);
    event(); assert(kb.text() == "4ё");
    // Touch can slide upward into the panel and select 4; no letter is inserted until release.
    open("", false, true, 1023); assert(kb.setLanguage("ru"));
    event(0, 0, true, 395, 285); now += KeyboardHold::delay;
    event(0, 0, true, 395, 285); assert(kb.text().empty());
    event(0, 0, true, 467, 250); // Cross the bridge between key and choices.
    event(0, 0, true, 467, 220); assert(kb.text().empty());
    event(); assert(kb.text() == "4");
    tap(48, 417); // Uppercase: Ё and 4 are both selectable.
    event(0, 0, true, 395, 285); now += KeyboardHold::delay;
    event(0, 0, true, 395, 285);
    previews.clear(); kb.draw(nullptr, true, "compose");
    assert((previews == std::vector<std::string>{"Ё", "4"}));
    event(0, 0, true, 467, 220); event(); assert(kb.text() == "44");
    // Leaving the panel cancels; returning before release restores a valid selection.
    tap(48, 417); // Numeric selection consumed the preceding one-shot Shift.
    event(0, 0, true, 395, 285); now += KeyboardHold::delay;
    event(0, 0, true, 395, 285); event(0, 0, true, 20, 100); event(); assert(kb.text() == "44");
    event(0, 0, true, 395, 285); now += KeyboardHold::delay;
    event(0, 0, true, 395, 285); event(0, 0, true, 20, 100);
    event(0, 0, true, 395, 220); event(); assert(kb.text() == "44Ё");
    // RU numbers 1–0 align to ц–х; е additionally keeps ё as the default.
    open("", false, true, 1023); assert(kb.setLanguage("ru"));
    for (int i = 1; i <= 10; ++i) {
        const int x = 17 + 85*i + 38;
        event(0, 0, true, x, 285); now += KeyboardHold::delay;
        event(0, 0, true, x, 285);
        if (i == 4) event(0, 0, true, x + 72, 220);
        event();
    }
    assert(kb.text() == "1234567890");
    // Punctuation has selectable lists too, with the same release and cancellation rules.
    open("", false, true, 1023);
    event(0, 0, true, 710, 505); now += KeyboardHold::delay;
    event(0, 0, true, 710, 505);
    previews.clear(); kb.draw(nullptr, false, "compose");
    assert((previews == std::vector<std::string>{"?", "!", "…"}));
    event(0, 0, true, 854, 438); event(); assert(kb.text() == "…");
    event(0, 0, true, 290, 505); now += KeyboardHold::delay;
    event(0, 0, true, 290, 505); event(0, 0, true, 362, 438); event(); assert(kb.text() == "…:");
    // Password choices expose only key alternatives; no enlarged character or entered text.
    open("", true, false, 1); assert(kb.setLanguage("ru"));
    event(0, 0, true, 395, 285); now += KeyboardHold::delay;
    event(0, 0, true, 395, 285); previews.clear(); kb.draw(nullptr, true, "unlock"); assert(previews.empty());
    event(0, 0, true, 467, 220); event(); assert(kb.text() == "4");
    event(0, 0, true, 395, 285); now += KeyboardHold::delay;
    event(); assert(kb.text() == "4"); // Default Ё cannot exceed the byte limit.
    // Real message policy: automatic case at the start, then lowercase.
    auto sentence = [&](const std::string& text = "", const std::string& code = "en", size_t limit = 1023) {
        kb.open(text, false, true, limit); assert(kb.setLanguage(code));
    };
    auto shift = [&] { tap(kb.language() == "ru" || kb.language() == "tr" ? 48 : 100, 417); };
    sentence(); tap(48, 285); tap(145, 285); assert(kb.text() == "Qw");
    sentence("word. "); tap(48, 285); tap(145, 285); assert(kb.text() == "word. Qw");
    sentence("word."); tap(48, 285); assert(kb.text() == "word.q");
    sentence("word"); tap(710, 505); button(SCE_CTRL_TRIANGLE); tap(48, 285);
    assert(kb.text() == "word. Q");
    button(SCE_CTRL_SQUARE); tap(145, 285); assert(kb.text() == "word. W");
    button(SCE_CTRL_SQUARE); button(SCE_CTRL_SQUARE); tap(145, 285); assert(kb.text() == "word.w");
    sentence("word\n"); tap(48, 285); assert(kb.text() == "word\nQ");
    sentence("Dr. "); tap(48, 285); assert(kb.text() == "Dr. q");
    // A single Shift is spent by one character, including a selected alternate or digit.
    sentence("word "); shift(); tap(48, 285); tap(145, 285); assert(kb.text() == "word Qw");
    sentence("word ", "ru"); shift();
    event(0, 0, true, 395, 285); now += KeyboardHold::delay; event();
    tap(648, 417); assert(kb.text() == "word Ёь");
    sentence("word ", "ru"); shift();
    event(0, 0, true, 395, 285); now += KeyboardHold::delay;
    event(0, 0, true, 395, 285); event(0, 0, true, 467, 220); event();
    tap(648, 417); assert(kb.text() == "word 4ь");
    // A cancelled choice or a rejected insertion must not spend Shift.
    sentence("word "); shift();
    event(0, 0, true, 48, 285); now += KeyboardHold::delay;
    event(0, 0, true, 20, 100); event(); tap(145, 285); tap(48, 285);
    assert(kb.text() == "word Wq");
    sentence("xy", "en", 2); shift(); tap(48, 285); assert(kb.text() == "xy");
    letter_keys.clear(); kb.draw(nullptr, true, "compose");
    assert(std::any_of(letter_keys.begin(), letter_keys.end(), [](const DrawnKey& key) { return key.label == "Q"; }));
    // Fast double tap locks; another Shift unlocks; an ordinary character breaks the pair.
    sentence("word "); shift(); shift();
    tap(48, 285); tap(145, 285); assert(kb.text() == "word QW");
    drawn.clear(); kb.draw(nullptr, true, "compose");
    assert(std::find(drawn.begin(), drawn.end(), "EN / CAPS") != drawn.end());
    shift(); tap(48, 285); assert(kb.text() == "word QWq");
    sentence("word "); shift(); now += KeyboardCase::doubleTapDelay + 1; shift();
    tap(48, 285); assert(kb.text() == "word q");
    sentence("word "); shift(); tap(48, 285); shift(); tap(145, 285); tap(48, 285);
    assert(kb.text() == "word QWq");
    // Automatic uppercase can be disabled for just the next character by tapping Shift.
    sentence(); shift(); tap(48, 285); assert(kb.text() == "q");
    sentence("word. "); shift(); tap(48, 285); assert(kb.text() == "word. q");
    // Controller × can double-tap Shift too; focus movement cannot consume Caps Lock.
    sentence("word "); button(SCE_CTRL_DOWN); button(SCE_CTRL_DOWN);
    button(SCE_CTRL_CROSS); button(SCE_CTRL_CROSS);
    button(SCE_CTRL_UP); button(SCE_CTRL_UP); button(SCE_CTRL_CROSS);
    button(SCE_CTRL_RIGHT); button(SCE_CTRL_CROSS); assert(kb.text() == "word QW");
    assert(kb.setLanguage("ru")); tap(395, 285); tap(648, 417); assert(kb.text() == "word QWЕЬ");
    // Locale-aware case also applies to automatic and one-shot Shift.
    sentence("", "tr"); tap(905, 350); tap(597, 285); assert(kb.text() == "İı");
    shift(); tap(905, 350); tap(905, 350); assert(kb.text() == "İıİi");
    sentence("", "fr"); tap(750, 417); // Auto capitalization must not turn apostrophe into '?'.
    assert(kb.text() == "'");
    // Passwords and non-message fields preserve their exact input case.
    kb.open("word. ", true, false, 128); assert(kb.setLanguage("en")); tap(48, 285); assert(kb.text() == "word. q");
    kb.open("", false, false, 1023); assert(kb.setLanguage("en")); tap(48, 285); assert(kb.text() == "q");
    // Shoulder buttons move the insertion point, square backspaces and triangle inserts space.
    open("aя😀z", false, true, 1023);
    assert(kb.cursor() == kb.text().size());
    button(SCE_CTRL_LTRIGGER); button(SCE_CTRL_LTRIGGER);
    assert(kb.cursor() == 3); // Before emoji, after the two-byte Cyrillic letter.
    button(SCE_CTRL_TRIANGLE); tap(48, 285);
    assert(kb.text() == "aя q😀z");
    button(SCE_CTRL_SQUARE); button(SCE_CTRL_SQUARE); button(SCE_CTRL_SQUARE);
    assert(kb.text() == "a😀z" && kb.cursor() == 1);
    button(SCE_CTRL_RTRIGGER); assert(kb.cursor() == 5);
    button(SCE_CTRL_SQUARE); assert(kb.text() == "az" && kb.cursor() == 1);
    button(SCE_CTRL_LTRIGGER); button(SCE_CTRL_LTRIGGER); button(SCE_CTRL_SQUARE);
    assert(kb.cursor() == 0 && kb.text() == "az");
    tap(145, 285); assert(kb.text() == "waz");
    for (int i = 0; i < 8; ++i) button(SCE_CTRL_RTRIGGER);
    assert(kb.cursor() == kb.text().size());
    // Capitalization uses the prefix at the cursor, preserving the text after it.
    sentence("word. tail");
    for (int i = 0; i < 4; ++i) button(SCE_CTRL_LTRIGGER);
    tap(48, 285); assert(kb.text() == "word. Qtail");
    button(SCE_CTRL_SQUARE); button(SCE_CTRL_TRIANGLE); tap(145, 285);
    assert(kb.text() == "word.  Wtail");
    // Newlines and selected alternatives are inserted at the same UTF-8 boundary.
    open("яz", false, true, 1023); assert(kb.setLanguage("ru"));
    button(SCE_CTRL_LTRIGGER); tap(770, 505);
    event(0, 0, true, 395, 285); now += KeyboardHold::delay; event();
    assert(kb.text() == "я\nёz");
    // Limits apply to the whole draft, not just the prefix; clear resets the cursor.
    open("я😀", false, false, 6); button(SCE_CTRL_LTRIGGER);
    button(SCE_CTRL_TRIANGLE); tap(48, 285); assert(kb.text() == "я😀" && kb.cursor() == 2);
    kb.clear(); assert(kb.cursor() == 0); tap(48, 285); assert(kb.text() == "q");
    open("aя", true, false, 128); button(SCE_CTRL_LTRIGGER);
    button(SCE_CTRL_TRIANGLE); assert(kb.text() == "a я");
    button(SCE_CTRL_SQUARE); assert(kb.text() == "aя");
    tap(750, 505); assert(kb.text().empty() && kb.cursor() == 0);
    // The visible caret follows wrapping and scrolls the five-line viewport.
    auto draw_caret = [&] {
        now = (now / 1000000 + 1) * 1000000;
        event(); carets.clear(); drawn.clear(); kb.draw(nullptr, true, "compose");
        assert(carets.size() == 1);
        return carets.front();
    };
    open(std::string(90, 'a'), false, true, 1023);
    for (int i = 0; i < 4; ++i) button(SCE_CTRL_LTRIGGER);
    assert(draw_caret() == std::make_pair(38, 113)); // Soft wrap at 86 characters.
    button(SCE_CTRL_LTRIGGER); assert(draw_caret() == std::make_pair(888, 87));
    open("zero\none\ntwo\nthree\nfour\nfive\nsix", false, true, 1023);
    assert(draw_caret() == std::make_pair(68, 191));
    assert(std::find(drawn.begin(), drawn.end(), "zero") == drawn.end());
    for (int i = 0; i < 40; ++i) button(SCE_CTRL_LTRIGGER);
    assert(draw_caret() == std::make_pair(38, 87));
    assert(std::find(drawn.begin(), drawn.end(), "zero") != drawn.end());
    assert(std::find(drawn.begin(), drawn.end(), "six") == drawn.end());
    // Masked input shows position only, never a substring of the passphrase.
    open("aя", true, false, 128); button(SCE_CTRL_LTRIGGER);
    const auto masked_caret = draw_caret();
    assert(masked_caret == std::make_pair(480, 205));
    for (const auto& line : drawn) assert(line.find("aя") == std::string::npos);
    // Passphrase controls are on the keys, without a separate shortcut-help strip.
    for (bool keyboard_only : {false, true}) {
        open("пароль", true, false, 128);
        drawn.clear(); rendered_text.clear(); shortcut_badges.clear();
        kb.draw(nullptr, true, "unlock", keyboard_only);
        assert(std::find(drawn.begin(), drawn.end(), i18n::tr("keyboard.help")) == drawn.end());
        for (int shortcut : {0, 1, 3, 5, 7, 8})
            assert(std::find(shortcut_badges.begin(), shortcut_badges.end(), shortcut) != shortcut_badges.end());
        assert(std::find(shortcut_badges.begin(), shortcut_badges.end(), 2) == shortcut_badges.end());
        for (const char* label : {"START", "L", "R"}) {
            const auto hint = std::find_if(rendered_text.begin(), rendered_text.end(), [&](const RenderedText& text) { return text.label == label; });
            assert(hint != rendered_text.end() && hint->baseline >= 478 && hint->baseline < 510);
        }
        for (const auto& line : drawn) assert(line.find("пароль") == std::string::npos);
    }
    // Backspace: immediate deletion, 350 ms initial delay, then one glyph every 60 ms.
    open("abя😀z", false, true, 1023);
    uint64_t deleting = now + 1000000;
    at(deleting, SCE_CTRL_SQUARE, SCE_CTRL_SQUARE); assert(kb.text() == "abя😀");
    at(deleting + 349999, 0, SCE_CTRL_SQUARE); assert(kb.text() == "abя😀");
    at(deleting + 350000, 0, SCE_CTRL_SQUARE); assert(kb.text() == "abя");
    at(deleting + 409999, 0, SCE_CTRL_SQUARE); assert(kb.text() == "abя");
    at(deleting + 410000, 0, SCE_CTRL_SQUARE); assert(kb.text() == "ab");
    at(deleting + 470000, 0, 0); assert(kb.text() == "ab"); // Release at the deadline does not delete.
    at(deleting + 2000000, 0, 0); assert(kb.text() == "ab");
    // A new tap starts a fresh delay; an overdue frame deletes at most one character.
    deleting = now + 1000000;
    at(deleting, SCE_CTRL_SQUARE, SCE_CTRL_SQUARE); assert(kb.text() == "a");
    at(deleting + 349999, 0, SCE_CTRL_SQUARE); assert(kb.text() == "a");
    at(deleting + 5000000, 0, SCE_CTRL_SQUARE); assert(kb.text().empty());
    at(deleting + 10000000, 0, SCE_CTRL_SQUARE); assert(kb.text().empty() && kb.cursor() == 0);
    event();
    open("abcdefgh", false, true, 1023); deleting = now + 1000000;
    at(deleting, SCE_CTRL_SQUARE, SCE_CTRL_SQUARE);
    at(deleting + 5000000, 0, SCE_CTRL_SQUARE); assert(kb.text() == "abcdef");
    at(deleting + 5000001, 0, SCE_CTRL_SQUARE); assert(kb.text() == "abcdef");
    event();
    // Touching Delete repeats, but leaving the key cancels until the next touch.
    open("abя😀", false, true, 1023); deleting = now + 1000000;
    at(deleting, 0, 0, true, 860, 417); assert(kb.text() == "abя");
    at(deleting + 350000, 0, 0, true, 860, 417); assert(kb.text() == "ab");
    at(deleting + 410000, 0, 0, true, 800, 200); assert(kb.text() == "ab");
    at(deleting + 1000000, 0, 0, true, 860, 417); assert(kb.text() == "ab");
    event();
    // Holding × on Delete works in a masked field and stops on focus movement.
    open("abcdef", true, false, 128); tap(860, 417); assert(kb.text() == "abcde");
    deleting = now + 1000000;
    at(deleting, SCE_CTRL_CROSS, SCE_CTRL_CROSS); assert(kb.text() == "abcd");
    at(deleting + 350000, 0, SCE_CTRL_CROSS); assert(kb.text() == "abc");
    at(deleting + 410000, SCE_CTRL_LEFT, SCE_CTRL_CROSS | SCE_CTRL_LEFT); assert(kb.text() == "abc");
    at(deleting + 1000000, 0, SCE_CTRL_CROSS); assert(kb.text() == "abc");
    event();
    // Repeating in the middle leaves the suffix intact, including UTF-8 characters.
    open("abя😀z", false, true, 1023); button(SCE_CTRL_LTRIGGER);
    deleting = now + 1000000;
    at(deleting, SCE_CTRL_SQUARE, SCE_CTRL_SQUARE); assert(kb.text() == "abяz");
    at(deleting + 350000, 0, SCE_CTRL_SQUARE); assert(kb.text() == "abz");
    at(deleting + 410000, 0, SCE_CTRL_SQUARE); assert(kb.text() == "az");
    at(deleting + 470000, 0, SCE_CTRL_SQUARE); assert(kb.text() == "z" && kb.cursor() == 0);
    at(deleting + 530000, 0, SCE_CTRL_SQUARE); assert(kb.text() == "z"); event();
    // Cancellation, cursor movement and a new dialog cannot inherit an old repeat.
    open("abcd", false, true, 1023); deleting = now + 1000000;
    at(deleting, SCE_CTRL_SQUARE, SCE_CTRL_SQUARE);
    assert(at(deleting + 350000, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE | SCE_CTRL_CIRCLE) == Result::Cancelled);
    at(deleting + 1000000, 0, SCE_CTRL_SQUARE); assert(kb.text() == "abc");
    open("next", false, true, 1023); at(deleting + 2000000, 0, SCE_CTRL_SQUARE); assert(kb.text() == "next");
    deleting = now + 1000000;
    at(deleting, SCE_CTRL_SQUARE, SCE_CTRL_SQUARE); assert(kb.text() == "nex");
    at(deleting + 350000, SCE_CTRL_LTRIGGER, SCE_CTRL_SQUARE | SCE_CTRL_LTRIGGER);
    at(deleting + 1000000, 0, SCE_CTRL_SQUARE); assert(kb.text() == "nex" && kb.cursor() == 2);
    kb.clear(); at(deleting + 2000000, 0, SCE_CTRL_SQUARE); assert(kb.text().empty()); event();
    // Two deletion sources starting together still erase just one character.
    open("abcd", false, true, 1023); tap(860, 417);
    event(SCE_CTRL_CROSS | SCE_CTRL_SQUARE, SCE_CTRL_CROSS | SCE_CTRL_SQUARE);
    assert(kb.text() == "ab"); event();
    // Quick taps cycle only enabled languages, without opening another screen.
    kb.setLanguages({"en", "ru", "de"});
    open("kept", false, true, 1023);
    const auto original_cursor = kb.cursor();
    tap(220, 505); assert(kb.language() == "ru");
    button(SCE_CTRL_CROSS); assert(kb.language() == "de"); // Focus remains on the language key.
    button(SCE_CTRL_CROSS); assert(kb.language() == "en");
    assert(kb.text() == "kept" && kb.cursor() == original_cursor);
    // The 300 ms boundary distinguishes cycling from selection; release commits once.
    event(0, 0, true, 220, 505); now += KeyboardHold::delay - 1001;
    event(); assert(kb.language() == "ru");
    event(); assert(kb.language() == "ru");
    event(0, 0, true, 220, 505); now += KeyboardHold::delay - 1000;
    event(); assert(kb.language() == "ru"); // Exactly 300 ms: current option, no quick cycle.
    event(0, 0, true, 220, 505); now += KeyboardHold::delay;
    event(0, 0, true, 220, 505);
    language_options.clear(); kb.draw(nullptr, true, "compose");
    assert((language_options == std::vector<std::string>{"RU", "DE", "EN"}));
    event(0, 0, true, 292, 438); event(); assert(kb.language() == "de" && kb.text() == "kept");
    // Circle cancels only the carousel; leaving the touch strip cancels selection.
    event(SCE_CTRL_CROSS, SCE_CTRL_CROSS); now += KeyboardHold::delay;
    event(0, SCE_CTRL_CROSS); event(SCE_CTRL_RIGHT, SCE_CTRL_CROSS | SCE_CTRL_RIGHT);
    assert(event(SCE_CTRL_CIRCLE, SCE_CTRL_CROSS | SCE_CTRL_CIRCLE) == Result::None);
    event(); assert(kb.language() == "de");
    event(0, 0, true, 220, 505); now += KeyboardHold::delay;
    event(0, 0, true, 20, 100); event(); assert(kb.language() == "de");
    // A single language is a no-op, including on the symbol layer.
    kb.setLanguages({"en"}); open("", false, true, 1023);
    tap(130, 505); tap(220, 505); tap(45, 285); assert(kb.text() == "1" && kb.language() == "en");
    // Language changes retain the insertion point and Caps Lock.
    kb.setLanguages({"en", "ru"}); sentence("draft"); shift(); shift();
    button(SCE_CTRL_LTRIGGER); const auto caps_cursor = kb.cursor();
    tap(220, 505); assert(kb.language() == "ru" && kb.cursor() == caps_cursor && kb.text() == "draft");
    drawn.clear(); kb.draw(nullptr, true, "compose");
    assert(std::find(drawn.begin(), drawn.end(), "RU / CAPS") != drawn.end());
    // Every profile is reachable by holding × and selecting in the bounded carousel.
    for (size_t i = 0; i < keyboard_layout::all().size(); ++i) {
        const auto& profile = keyboard_layout::all()[i];
        CustomKeyboard picker;
        picker.setLanguages(keyboard_languages::supported());
        picker.open("draft", false, true, 1023, true); assert(picker.setLanguage("en"));
        auto frame = [&](unsigned pressed, unsigned held, bool touching, int x, int y) {
            return picker.handle(pressed, held, touching, x, y, ++now);
        };
        for (int row = 0; row < 3; ++row) frame(SCE_CTRL_DOWN, SCE_CTRL_DOWN, false, 0, 0);
        frame(SCE_CTRL_RIGHT, SCE_CTRL_RIGHT, false, 0, 0); // Symbols -> language on the bottom row.
        frame(SCE_CTRL_CROSS, SCE_CTRL_CROSS, false, 0, 0); now += KeyboardHold::delay;
        frame(0, SCE_CTRL_CROSS, false, 0, 0);
        for (size_t option = 0; option < i; ++option)
            frame(SCE_CTRL_RIGHT, SCE_CTRL_CROSS | SCE_CTRL_RIGHT, false, 0, 0);
        language_options.clear(); picker.draw(nullptr, true, "compose");
        assert(language_options.size() == 7);
        assert(std::find(language_options.begin(), language_options.end(), profile.badge) != language_options.end());
        assert(picker.language() == "en");
        const auto chosen = frame(0, 0, false, 0, 0);
        assert(chosen == (profile.system() ? Result::System : Result::None));
        assert(picker.language() == profile.code && picker.text() == "draft");
        if (profile.system()) continue;
        letter_keys.clear(); picker.draw(nullptr, true, "compose");
        const auto first_popup = std::find_if(letter_keys.begin(), letter_keys.end(), [&](const DrawnKey& key) {
            return profile.popups.count(key.label) && profile.popups.at(key.label).size();
        });
        assert(first_popup != letter_keys.end());
        const auto target = *first_popup;
        frame(0, 0, true, target.x, target.baseline - 9); now += KeyboardHold::delay;
        frame(0, 0, true, target.x, target.baseline - 9); assert(picker.text() == "draft");
        frame(0, 0, false, 0, 0);
        assert(picker.text() == "draft" + profile.popups.at(target.label).front());
        picker.open("next", false, true, 1023, true); assert(picker.language() == profile.code);
    }
    // Touch edge scrolling exposes language options beyond the first seven cells.
    kb.setLanguages(keyboard_languages::supported()); open("draft", false, true, 1023);
    event(0, 0, true, 220, 505); now += KeyboardHold::delay;
    event(0, 0, true, 220, 505); event(0, 0, true, 652, 438);
    now += 350000; event(0, 0, true, 652, 438); // Window advances by one to RU..PT-PT.
    language_options.clear(); kb.draw(nullptr, true, "compose");
    assert(language_options.front() == "RU" && language_options.back() == "PT");
    event(); assert(kb.language() == "pt-PT" && kb.text() == "draft");
    // Controller selection wraps from the current profile to the final enabled one.
    kb.setLanguages({"en", "ru", "de"}); open("draft", false, true, 1023);
    tap(220, 505); // RU, retaining focus.
    event(SCE_CTRL_CROSS, SCE_CTRL_CROSS); now += KeyboardHold::delay;
    event(0, SCE_CTRL_CROSS); event(SCE_CTRL_LEFT, SCE_CTRL_CROSS | SCE_CTRL_LEFT);
    event(); assert(kb.language() == "en");
    // Native routing preserves the existing UTF-16 draft and explicitly forces the requested IME.
    const uint64_t masks[] = {SCE_IME_LANGUAGE_JAPANESE, SCE_IME_LANGUAGE_KOREAN,
        SCE_IME_LANGUAGE_SIMPLIFIED_CHINESE, SCE_IME_LANGUAGE_TRADITIONAL_CHINESE};
    for (int i = 0; i < 4; ++i) {
        Keyboard fallback; fallback.configure(true, true, {"en", keyboard_layout::all()[15+i].code}); ime_finished = false;
        const int previous_inits = ime_inits;
        fallback.open("compose", false, "draft😀", true);
        fallback.handle(0, 0, false, 0, 0, ++now); // Release opening gesture.
        fallback.handle(0, 0, true, 220, 505, ++now);
        fallback.handle(0, 0, false, 0, 0, ++now); // Short release switches directly to the enabled IME language.
    assert(fallback.systemActive() && !fallback.ownsScreen() && ime_inits == previous_inits + 1);
        assert(last_ime.supportedLanguages == masks[i] && last_ime.languagesForced == SCE_TRUE);
        assert(last_ime.initialText[0] == 'd' && last_ime.initialText[4] == 't');
        assert(last_ime.initialText[5] == 0xd83d && last_ime.initialText[6] == 0xde00);
        assert(!(last_ime.option & SCE_IME_OPTION_MULTILINE));
        fallback.close();
    }
    Keyboard password; password.configure(true, true, {"en", "ja"}); password.open("password", true, "draft");
    password.handle(0, 0, false, 0, 0, ++now);
    password.handle(0, 0, true, 255, 505, ++now); password.handle(0, 0, false, 0, 0, ++now);
    assert(password.systemActive() && last_ime.textBoxMode == SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD);
    assert((last_ime.option & (SCE_IME_OPTION_NO_ASSISTANCE | SCE_IME_OPTION_NO_AUTO_CAPITALIZATION)) ==
        (SCE_IME_OPTION_NO_ASSISTANCE | SCE_IME_OPTION_NO_AUTO_CAPITALIZATION));
    password.close();
    // The console language, not a manually overridden interface, determines defaults.
    i18n::init(argv[1], "fr");
    CustomKeyboard automatic; automatic.open("", false, true, 1023);
    assert(automatic.language() == "en" && !automatic.setLanguage("fr")); // Host has no Vita system language.
    Keyboard english; english.configure(false, true); english.open("compose");
    assert(english.systemActive());
    assert(last_ime.supportedLanguages == (SCE_IME_LANGUAGE_ENGLISH | SCE_IME_LANGUAGE_ENGLISH_GB));
    english.close();
    CustomKeyboard restricted;
    restricted.setLanguages({"en", "ru"}); restricted.open("draft", false, true, 1023, true);
    assert(!restricted.setLanguage("fr") && restricted.setLanguage("ru"));
    restricted.handle(SCE_CTRL_LTRIGGER, SCE_CTRL_LTRIGGER, false, 0, 0, ++now);
    const size_t draft_cursor = restricted.cursor();
    restricted.handle(0, 0, true, 220, 505, ++now);
    restricted.handle(0, 0, false, 0, 0, ++now); // Short release cycles RU -> EN.
    assert(restricted.language() == "en" && restricted.text() == "draft" && restricted.cursor() == draft_cursor);
    restricted.setLanguages({"ru"}); restricted.open("next", false, true, 1023);
    assert(restricted.language() == "ru" && !restricted.setLanguage("en"));
    Keyboard native; native.configure(false, true, {"en", "ru", "tr", "pt-BR"}); native.open("compose");
    assert(last_ime.supportedLanguages == (SCE_IME_LANGUAGE_ENGLISH | SCE_IME_LANGUAGE_ENGLISH_GB |
        SCE_IME_LANGUAGE_RUSSIAN | SCE_IME_LANGUAGE_TURKISH | SCE_IME_LANGUAGE_PORTUGUESE_BR));
    native.close();
    Keyboard japanese; japanese.configure(true, true, {"ja"}); japanese.open("compose", false, "draft", true);
    assert(japanese.systemActive() && last_ime.supportedLanguages == SCE_IME_LANGUAGE_JAPANESE);
    assert(!(last_ime.option & SCE_IME_OPTION_MULTILINE));
    japanese.close();
    CustomKeyboard vault_keyboard; vault_keyboard.setLanguages({"ja", "ko"}); vault_keyboard.open("", true, false, 128);
    assert(!vault_keyboard.systemRequested() && !vault_keyboard.setLanguage("ja"));
    assert(vault_keyboard.language() == "en");
    i18n::select("en");
    // Changing the active palette changes the keyboard, including pressed and disabled glyph contrast.
    for (const auto accent : {RGBA8(54,158,255,255),RGBA8(255,54,93,255),RGBA8(255,218,0,255)}) {
        palette={{"background",RGBA8(5,8,15,255)},{"menu",RGBA8(13,17,23,255)},
            {"surface",RGBA8(22,14,20,255)},{"surface_secondary",RGBA8(41,17,26,255)},
            {"selected",RGBA8(64,21,33,255)},{"text",RGBA8(255,243,246,255)},
            {"muted",RGBA8(218,191,201,255)},{"accent",accent},{"focus",RGBA8(200,9,9,255)},
            {"border",RGBA8(87,78,83,255)},{"header_text",RGBA8(8,6,9,255)}};
        open("draft",false,true,1023);
        background_calls=header_calls=key_slices=0;rendered_text.clear();fills.clear();
        shapes.clear();record_shapes=true;smooth_contours=0;
        kb.draw(nullptr,false,"themed"); // Palette decides brightness even when the caller passes Light.
        assert(background_calls==1 && header_calls==1 && key_slices>20);
        record_shapes=false;assert(smooth_contours>=60);
        const auto outline=std::find_if(shapes.begin(),shapes.end(),[&](const auto& shape){return shape.color==palette["focus"];});
        assert(outline!=shapes.end() && outline+1!=shapes.end());
        const auto face=outline+1;
        assert(std::fabs(face->x-outline->x-2)<.001f && std::fabs(face->y-outline->y-2)<.001f);
        assert(std::fabs(outline->w-face->w-4)<.001f && std::fabs(outline->h-face->h-4)<.001f);
        assert(std::find(fills.begin(),fills.end(),palette["focus"])!=fills.end());
        auto label_color=[&](const std::string& label){
            const auto it=std::find_if(rendered_text.begin(),rendered_text.end(),[&](const auto& run){return run.label==label;});
            assert(it!=rendered_text.end());return it->color;
        };
        assert(label_color("themed")==palette["header_text"]);
        assert(ui_color::contrast(label_color("q"),palette["surface"])>=4.5);
        assert(label_color(i18n::tr("keyboard.send"))==ui_color::on(accent));
        assert(ui_color::contrast(label_color(i18n::tr("keyboard.send")),accent)>=4.5);
        event(0,0,true,890,505);rendered_text.clear();kb.draw(nullptr,false,"themed");
        assert(ui_color::contrast(label_color(i18n::tr("keyboard.send")),palette["selected"])>=4.5);
        event();open("",false,true,1023);rendered_text.clear();kb.draw(nullptr,false,"themed");
        assert(ui_color::contrast(label_color(i18n::tr("keyboard.send")),palette["surface_secondary"])>=4.5);
        background_calls=header_calls=0;open("secret",true,false,128);rendered_text.clear();
        kb.draw(nullptr,false,"unlock",true);
        assert(background_calls==0 && header_calls==0);
        for(const auto& run:rendered_text)assert(run.label.find("secret")==std::string::npos);
    }
    palette.clear();
    // Real two-contact packets reach the shared keyboard; the whole chord owns its release tail.
    CustomKeyboard split_keyboard; split_keyboard.setSplitLevel(0); split_keyboard.setLanguages(keyboard_languages::supported());
    split_keyboard.open("draft",false,true,1023,false,false);assert(split_keyboard.setLanguage("en"));
    struct Contact { unsigned char id; int x,y; };
    uint64_t gesture_now=100000;
    auto fingers=[&](std::initializer_list<Contact> contacts,uint64_t elapsed=16000,unsigned pressed=0){
        SceTouchData data{};
        for(const auto point:contacts){auto& r=data.report[data.reportNum++];r.id=point.id;r.x=point.x*2;r.y=point.y*2;}
        gesture_now+=elapsed;
        return split_keyboard.handle(pressed,pressed,data.reportNum!=0,data.report[0].x/2,data.report[0].y/2,gesture_now,&data);
    };
    fingers({{7,450,350}});assert(split_keyboard.text()=="draft"); // A plain letter waits for release.
    fingers({{7,450,350},{9,510,350}});
    fingers({{9,560,350},{7,400,350}});assert(!split_keyboard.split()); // Below threshold, reordered reports.
    fingers({{9,610,350},{7,350,350}});assert(split_keyboard.split());
    fingers({{7,450,350},{9,510,350}});assert(split_keyboard.split()); // One toggle per chord.
    fingers({{7,50,280}});fingers({});assert(split_keyboard.text()=="draft" && split_keyboard.cursor()==5);
    letter_keys.clear();split_keyboard.draw(nullptr,true,"split");
    for(const auto& key:letter_keys)if(key.baseline<478)assert(key.x<410 || key.x>550);
    fingers({{3,480,350}});fingers({});assert(split_keyboard.text()=="draft"); // Empty center.
    fingers({{3,480,505}});fingers({});assert(split_keyboard.text()=="draft "); // Wide spacebar retained.
    for(const auto& profile:keyboard_layout::all())if(!profile.system()) {
        assert(split_keyboard.setLanguage(profile.code));
        letter_keys.clear();split_keyboard.draw(nullptr,true,"split");
        assert(!letter_keys.empty());
        for(const auto& key:letter_keys)if(key.baseline<478)assert(key.x>=16 && key.x<=944 && (key.x<410 || key.x>550));
    }
    // Each spread uses the drawn geometry for typing; key faces stay disjoint and inside the screen.
    for (int level = 0; level < 3; ++level) {
        split_keyboard.setSplitLevel(level);
        split_keyboard.open("",false,true,1023,false,false); // Reopening preserves the spread.
        for (const auto& profile : keyboard_layout::all()) if (!profile.system()) {
            split_keyboard.open("",false,true,1023,false,false);
            assert(split_keyboard.setLanguage(profile.code));
            for (bool dark : {false, true}) {
                letter_keys.clear(); shapes.clear(); record_shapes=true;
                split_keyboard.draw(nullptr,dark,"spread"); record_shapes=false;
                std::vector<Shape> faces;
                for (const auto& shape : shapes) if (shape.h == 64 && shape.y < 478) {
                    assert(shape.x >= 16 && shape.x + shape.w <= 944);
                    assert(shape.x + shape.w <= 400-80*level || shape.x >= 560+80*level);
                    for (const auto& previous : faces)
                        assert(shape.y != previous.y || shape.x >= previous.x+previous.w || previous.x >= shape.x+shape.w);
                    faces.push_back(shape);
                }
                assert(!faces.empty());
                if (profile.code == "en") {
                    const int widths[] = {64, 54, 38};
                    assert(faces.front().w == widths[level]);
                }
                const auto keys=letter_keys;
                for (const auto& key : keys) if (key.baseline < 478) {
                    const auto before=split_keyboard.text();
                    fingers({{3,key.x,key.baseline-20}}); fingers({});
                    assert(split_keyboard.text() == before+key.label);
                }
            }
            const auto before=split_keyboard.text();
            fingers({{3,480,350}}); fingers({}); assert(split_keyboard.text()==before);
        }
    }
    // A fresh keyboard defaults to the same geometry as explicit medium.
    CustomKeyboard medium; medium.setLanguages({"en"}); medium.open("",false,true,1023,false,false);
    SceTouchData spread{}; spread.reportNum=2;
    spread.report[0].id=1; spread.report[0].x=700; spread.report[0].y=700;
    spread.report[1].id=2; spread.report[1].x=1220; spread.report[1].y=700;
    medium.handle(0,0,true,350,350,++gesture_now,&spread);
    spread.report[0].x=400; spread.report[1].x=1520;
    medium.handle(0,0,true,200,350,++gesture_now,&spread); assert(medium.split());
    letter_keys.clear(); medium.draw(nullptr,true,"default"); const auto default_keys=letter_keys;
    medium.setSplitLevel(1); letter_keys.clear(); medium.draw(nullptr,true,"medium");
    assert(letter_keys.size()==default_keys.size());
    for (size_t i=0;i<letter_keys.size();++i) assert(letter_keys[i].x==default_keys[i].x);
    split_keyboard.setSplitLevel(0);
    assert(split_keyboard.setLanguage("en"));
    split_keyboard.open("secret",true,false,128,false,false);assert(split_keyboard.split());
    drawn.clear();split_keyboard.draw(nullptr,true,"unlock",true);
    for(const auto& line:drawn)assert(line.find("secret")==std::string::npos);
    fingers({{2,120,350},{6,840,350}});fingers({{6,740,350},{2,220,350}});assert(!split_keyboard.split());
    fingers({{6,740,350}});fingers({});assert(split_keyboard.text()=="secret");
    split_keyboard.open("draft",false,true,1023,false,false);
    // Parallel/vertical drags, one moving finger, changed IDs, third fingers and stale chords cannot switch modes.
    fingers({{1,350,350},{2,610,350}});fingers({{1,250,350},{2,510,350}});fingers({});assert(!split_keyboard.split());
    fingers({{1,350,350},{2,610,350}});fingers({{1,350,250},{2,610,450}});fingers({});assert(!split_keyboard.split());
    fingers({{1,350,350},{2,610,350}});fingers({{1,200,350},{2,610,350}});fingers({});assert(!split_keyboard.split());
    fingers({{1,350,350},{2,610,350}});fingers({{3,200,350},{2,760,350}});fingers({});assert(!split_keyboard.split());
    fingers({{1,350,350},{2,610,350}});fingers({{1,200,350},{2,760,350},{3,480,350}});fingers({});assert(!split_keyboard.split());
    fingers({{1,350,350},{2,610,350}});fingers({{1,200,350},{2,760,350}},1600000);fingers({});assert(!split_keyboard.split());
    fingers({{1,450,350}});fingers({{1,450,350},{2,510,350}},200000);fingers({{1,350,350},{2,610,350}});fingers({});assert(!split_keyboard.split());
    split_keyboard.open("draft",false,true,1023,false,false);
    // Single-finger input still works, including release submission and repeat deletion.
    fingers({{1,450,350}});fingers({});assert(split_keyboard.text()=="draftg");
    fingers({{1,900,430}});fingers({});assert(split_keyboard.text()=="draft");
    fingers({{1,900,430}});fingers({{1,900,430}},350000);assert(split_keyboard.text()=="draf");
    fingers({{1,900,430}},60000);assert(split_keyboard.text()=="dra");fingers({});assert(split_keyboard.text()=="dra");
    assert(fingers({{1,890,505}})==Result::None);assert(fingers({})==Result::Submitted);
    // The public Keyboard wrapper forwards both contacts as well.
    Keyboard split_wrapper;split_wrapper.configure(true,true,{"en"});split_wrapper.open("compose",false,"draft",true);
    split_wrapper.handle(0,0,false,0,0,++gesture_now);
    SceTouchData chord{};chord.reportNum=2;chord.report[0].id=1;chord.report[0].x=700;chord.report[0].y=700;
    chord.report[1].id=2;chord.report[1].x=1220;chord.report[1].y=700;
    split_wrapper.handle(0,0,true,350,350,++gesture_now,&chord);
    chord.report[0].x=400;chord.report[1].x=1520;
    split_wrapper.handle(0,0,true,200,350,++gesture_now,&chord);
    chord.reportNum=0;split_wrapper.handle(0,0,false,0,0,++gesture_now,&chord);
    letter_keys.clear();split_wrapper.draw(nullptr);
    for(const auto& key:letter_keys)if(key.baseline<478)assert(key.x<410 || key.x>550);
    split_wrapper.close();
    // Contact transitions must not require an empty frame or follow report-array order.
    auto reset_typing = [&] {
        split_keyboard.open("",false,true,1023,false,false);
        assert(split_keyboard.setLanguage("en"));
    };
    reset_typing();
    fingers({{1,48,285}}); fingers({{2,135,285}}); fingers({});
    assert(split_keyboard.text()=="qw");
    reset_typing();
    fingers({{1,48,285}}); fingers({{1,48,285},{2,135,285}});
    fingers({{2,135,285},{1,48,285}}); fingers({{1,48,285}}); fingers({});
    assert(split_keyboard.text()=="qw");
    reset_typing();
    fingers({{1,48,285},{2,135,285}}); fingers({});
    assert(split_keyboard.text()=="qw");
    reset_typing();
    fingers({{1,48,285}}); fingers({}); fingers({{1,135,285}}); fingers({});
    assert(split_keyboard.text()=="qw");
    reset_typing();
    fingers({{1,450,350}}); fingers({{1,450,350},{2,550,350}});
    fingers({{2,550,350}}); fingers({});
    assert(!split_keyboard.split() && split_keyboard.text()=="gh"); // Failed gesture still types.
    reset_typing();
    fingers({{1,450,350},{2,550,350}}); fingers({});
    assert(!split_keyboard.split() && split_keyboard.text()=="gh");
    open(std::string(600,'a'),false,true,1023);
    width_calls=0; kb.draw(nullptr,true,"draft");
    const auto first_draw=width_calls;
    kb.draw(nullptr,true,"draft");
    const auto cached_draw=width_calls-first_draw;
    assert(first_draw>cached_draw+600);
    std::printf("Draft width measurements for 600 characters: first frame=%zu, unchanged frame=%zu\n",first_draw,cached_draw);
    ++font_revision;
    const auto before_font_change=width_calls; kb.draw(nullptr,true,"draft");
    assert(width_calls-before_font_change>cached_draw+600);
    button(SCE_CTRL_LTRIGGER);
    const auto before_cursor_change=width_calls; kb.draw(nullptr,true,"draft");
    assert(width_calls-before_cursor_change>cached_draw+600);
    button(SCE_CTRL_SQUARE);
    const auto before_edit=width_calls; kb.draw(nullptr,true,"draft");
    assert(width_calls-before_edit>cached_draw+599);
    open("",false,true,1023);
    palette["background"]=RGBA8(12,23,34,40);
    fills.clear(); kb.draw(nullptr,true,"opaque keyboard");
    assert(!fills.empty() && fills.front()==RGBA8(12,23,34,255));
    palette.clear();
    // A complete tap between UI frames must survive a slow render, without duplicate insertion.
    auto packet=[](uint64_t time,int id=-1,int x=0,int y=0) {
        SceTouchData sample{}; sample.timeStamp=time;
        if (id>=0) { sample.reportNum=1; sample.report[0].id=id; sample.report[0].x=x*2; sample.report[0].y=y*2; }
        return sample;
    };
    open("",false,false,1023);
    auto initial=packet(1000);
    kb.handle(0,0,false,0,0,1000,&initial);
    touch_backlog={packet(2000,1,48,285),packet(18000),packet(34000,2,135,285),packet(50000)};
    auto latest=touch_backlog.back();
    kb.handle(0,0,false,0,0,160000,&latest);
    assert(kb.text()=="qw" && touch_backlog.empty());
    const auto reads=touch_reads;
    kb.handle(0,0,false,0,0,176000,&latest);
    assert(kb.text()=="qw" && touch_reads==reads);
    open("",false,false,1023);
    kb.handle(0,0,false,0,0,1000,&initial);
    touch_backlog={packet(2000,1,48,285),packet(302000,1,48,285),packet(320000)};
    latest=touch_backlog.back();
    kb.handle(0,0,false,0,0,500000,&latest);
    assert(kb.text()=="1"); // Long-press duration comes from samples, not one UI-frame time.
    TouchHistory history;
    history.ignore(&initial);
    auto newest=packet(10000);
    assert(history.poll(&newest,[&](SceTouchData* out,unsigned) {
        out[0]=packet(8000); out[1]=packet(2000); out[2]=packet(8000); return 3;
    })==3);
    assert(history.samples[0].timeStamp==2000 && history.samples[1].timeStamp==8000 && history.samples[2].timeStamp==10000);
    Keyboard filtered; filtered.configure(true,true,{"en"}); filtered.open("test");
    auto released=packet(2000);
    filtered.handle(0,0,false,0,0,2000,&released);
    touch_backlog={packet(1000,1,48,285),packet(2000),packet(3500,2,135,285),packet(5000)};
    latest=touch_backlog.back();
    filtered.handle(0,0,false,0,0,160000,&latest);
    filtered.handle(SCE_CTRL_START,SCE_CTRL_START,false,0,0,176000,&latest);
    assert(filtered.submitted() && filtered.text()=="w");
    filtered.close();
    auto future=packet(12000);
    assert(history.poll(&future,[](SceTouchData*,unsigned) { return -1; })==1);
    assert(history.samples[0].timeStamp==12000);
    history.ignore(nullptr);
    assert(history.samples[0].timeStamp==0 && history.samples[0].reportNum==0);
    history.ignore(&initial);
    auto full_latest=packet(200000);
    assert(history.poll(&full_latest,[&](SceTouchData* data,unsigned count) {
        assert(count==64);
        for (unsigned i=0;i<count;++i) data[i]=packet(2000+i*1000);
        return count;
    })==65);
    assert(history.samples[64].timeStamp==full_latest.timeStamp);
    char pattern[] = "/tmp/vitagram-custom-keyboard-XXXXXX";
    const std::string custom = mkdtemp(pattern);
    std::ifstream source(std::string(argv[1]) + "/en.lang");
    std::string catalog((std::istreambuf_iterator<char>(source)), {});
    catalog.replace(catalog.find("@code = en"), 10, "@code = xx");
    std::ofstream(custom + "/xx.lang") << catalog;
    i18n::init(argv[1], "en", custom);
    CustomKeyboard added;
    added.setLanguages({"xx"}); added.open("", false, true, 1023, false, false);
    assert(added.language() == "xx" && added.setLanguage("xx"));
    added.handle(0,0,true,52,290,++now); added.handle(0,0,false,0,0,++now);
    assert(added.text() == "q");
    Keyboard native_added;
    native_added.configure(false, true, {"xx"}); native_added.open("custom");
    assert(native_added.systemActive() && last_ime.supportedLanguages == SCE_IME_LANGUAGE_ENGLISH);
    native_added.close();
    std::filesystem::remove_all(custom);
    std::puts("custom keyboard: touch/controller, split gestures, draft cache and touch history across slow frames: PASS");
}
