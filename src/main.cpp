#include "telegram_client.h"
#include "keyboard.h"
#include "vita_net.h"
#include "media_render.h"
#include "animation_player.h"
#include "voice_player.h"
#include "cpu_scheduler.h"
#include "settings.h"
#include "photo_view.h"
#include <psp2/rtc.h>
#include <ctime>

#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <pthread.h>
#include <vita2d.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

struct CachedTexture {
    std::string path;
    vita2d_texture* texture = nullptr;
    unsigned int used = 0;
    size_t bytes = 0;
};

class TextureCache {
public:
    void beginFrame(bool pause) { paused_ = pause; requests_.clear(); }
    vita2d_texture* get(const std::string& path) {
        if (path.empty()) return nullptr;
        ++clock_;
        for (auto& item : items_) {
            if (item.path == path) {
                item.used = clock_;
                return item.texture;
            }
        }
        if (std::find(requests_.begin(), requests_.end(), path) == requests_.end()) requests_.push_back(path);
        return nullptr;
    }
    void loadOne() {
        if (paused_ || requests_.empty()) return;
        const std::string path = requests_.front();
        // Upload/free textures only after the previous frame has finished using them.
        vita2d_wait_rendering_done();
        vita2d_texture* texture = nullptr;
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".png")
            texture = vita2d_load_PNG_file(path.c_str());
        else if ((lower.size() >= 4 && lower.substr(lower.size() - 4) == ".jpg") ||
                 (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".jpeg"))
            texture = vita2d_load_JPEG_file(path.c_str());
        else texture = load_webp(path.c_str());
        size_t bytes = texture ? (size_t)vita2d_texture_get_stride(texture) * vita2d_texture_get_height(texture) : 0;
        if (bytes > 16 * 1024 * 1024) { vita2d_free_texture(texture); texture = nullptr; bytes = 0; }
        // 16 MiB of pixel storage, rather than eight textures of arbitrary size.
        while (!items_.empty() && (used_bytes_ + bytes > 16 * 1024 * 1024 || items_.size() >= 256)) {
            auto oldest = std::min_element(items_.begin(), items_.end(), [](const CachedTexture& a, const CachedTexture& b) {
                return a.used < b.used;
            });
            if (oldest->texture) vita2d_free_texture(oldest->texture);
            used_bytes_ -= oldest->bytes;
            items_.erase(oldest);
        }
        items_.push_back({path, texture, clock_, bytes});
        used_bytes_ += bytes;
    }
    void clear() {
        for (auto& item : items_) if (item.texture) vita2d_free_texture(item.texture);
        items_.clear();
        used_bytes_ = 0;
    }
private:
    bool paused_ = false;
    size_t used_bytes_ = 0;
    std::vector<std::string> requests_;
    std::vector<CachedTexture> items_;
    unsigned int clock_ = 0;
};

static void draw_texture_fit(vita2d_texture* texture, float x, float y, float max_w, float max_h) {
    if (!texture) return;
    float w = (float)vita2d_texture_get_width(texture);
    float h = (float)vita2d_texture_get_height(texture);
    float scale = std::min(max_w / w, max_h / h);
    vita2d_draw_texture_scale(texture, x, y, scale, scale);
}

static void draw_text(vita2d_pgf* f, int x, int y, const std::string& s, unsigned int color, float scale = 0.8f) {
    rich_text_draw(f, x, y, color, scale, s.c_str());
}

static std::string clip(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t end = n > 3 ? n - 3 : n;
    while (end && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80) --end;
    return s.substr(0, end) + "...";
}

static void pop_utf8(std::string& text) {
    if (text.empty()) return;
    size_t start = text.size() - 1;
    while (start && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) --start;
    text.resize(start);
}

static std::string fit_text(vita2d_pgf* font, std::string text, int width, float scale) {
    if (rich_text_width(font, scale, text.c_str()) <= width) return text;
    while (!text.empty() && rich_text_width(font, scale, (text + "...").c_str()) > width) pop_utf8(text);
    return text + "...";
}

static std::vector<std::string> wrap_text(vita2d_pgf* font, const std::string& text, int width) {
    struct Entry { std::string text; int width; std::vector<std::string> lines; };
    static std::vector<Entry> cache;
    static int font_probe = 0;
    int probe = rich_text_width(font, 1.0f, "Font size");
    if (probe != font_probe) { cache.clear(); font_probe = probe; }
    for (const auto& entry : cache) if (entry.width == width && entry.text == text) return entry.lines;
    std::vector<std::string> lines;
    std::istringstream paragraphs(text);
    std::string paragraph;
    while (std::getline(paragraphs, paragraph)) {
    std::istringstream input(paragraph);
    std::string word, line;
    while (input >> word) {
        if (!line.empty() && rich_text_width(font, 0.68f, (line + " " + word).c_str()) > width) {
            lines.push_back(line);
            line.clear();
        }
        // Split long URLs/words only on UTF-8 character boundaries.
        if (rich_text_width(font, 0.68f, word.c_str()) > width) {
            for (size_t i = 0; i < word.size();) {
                size_t end = i + 1;
                while (end < word.size() && (static_cast<unsigned char>(word[end]) & 0xc0) == 0x80) ++end;
                std::string glyph = word.substr(i, end - i);
                if (!line.empty() && rich_text_width(font, 0.68f, (line + glyph).c_str()) > width) {
                    lines.push_back(line); line.clear();
                }
                line += glyph; i = end;
            }
            continue;
        }
        if (!line.empty()) line += ' ';
        line += word;
    }
    lines.push_back(line);
    }
    if (lines.empty()) lines.push_back("");
    if (cache.size() >= 16) cache.erase(cache.begin());
    cache.push_back({text, width, lines});
    return lines;
}

static void rounded_rect(float x, float y, float w, float h, unsigned int color) {
    const float r = 10;
    vita2d_draw_rectangle(x + r, y, w - 2*r, h, color);
    vita2d_draw_rectangle(x, y + r, w, h - 2*r, color);
    vita2d_draw_fill_circle(x+r, y+r, r, color);
    vita2d_draw_fill_circle(x+w-r, y+r, r, color);
    vita2d_draw_fill_circle(x+r, y+h-r, r, color);
    vita2d_draw_fill_circle(x+w-r, y+h-r, r, color);
}

struct PostLine { std::string text; std::string url; };

static std::string message_time(const MessageItem& message, bool full = false) {
    if (message.date <= 0) return "--:--";
    SceDateTime date{};
    SceRtcTick utc{}, local{};
    if (sceRtcSetTime_t(&date, static_cast<time_t>(message.date)) < 0 || sceRtcGetTick(&date, &utc) < 0)
        return "--:--";
    bool local_ok = sceRtcConvertUtcToLocalTime(&utc, &local) >= 0 && sceRtcSetTick(&date, &local) >= 0;
    char text[64];
    if (full) std::snprintf(text, sizeof(text), "%02u.%02u.%04u  %02u:%02u%s%s",
        (unsigned)date.day, (unsigned)date.month, (unsigned)date.year, (unsigned)date.hour, (unsigned)date.minute,
        local_ok ? "" : " UTC", message.edit_date ? " (edited)" : "");
    else std::snprintf(text, sizeof(text), "%02u:%02u%s", (unsigned)date.hour, (unsigned)date.minute, local_ok ? "" : " UTC");
    return text;
}

static std::string message_day(const MessageItem& message) {
    if (message.date <= 0) return "Дата неизвестна";
    SceDateTime date{}; SceRtcTick utc{}, local{};
    if (sceRtcSetTime_t(&date, static_cast<time_t>(message.date)) < 0 || sceRtcGetTick(&date, &utc) < 0) return "Дата неизвестна";
    bool local_ok = sceRtcConvertUtcToLocalTime(&utc, &local) >= 0 && sceRtcSetTick(&date, &local) >= 0;
    static const char* months[] = {"января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа", "сентября", "октября", "ноября", "декабря"};
    if (date.month < 1 || date.month > 12) return "Дата неизвестна";
    return std::to_string(date.day) + " " + months[date.month-1] + " " + std::to_string(date.year) + (local_ok ? "" : " UTC");
}

struct HistoryRow { int index, top, height, day_top; std::string day; };
// Shared by drawing and touch hit testing; date pills are not messages.
static std::vector<HistoryRow> history_layout(const std::vector<MessageItem>& messages, int selected, bool group) {
    if (messages.empty()) return {};
    selected = std::max(0, std::min(selected, (int)messages.size()-1));
    int begin = std::max(0, selected-5);
    for (;;) {
        std::vector<HistoryRow> rows;
        int y = 40;
        std::string previous;
        for (int i = begin; i < (int)messages.size(); ++i) {
            std::string day = message_day(messages[i]);
            bool separator = day != previous;
            int height = group && !messages[i].outgoing ? 98 : 78;
            int day_top = separator ? y : -1;
            int top = y + (separator ? 26 : 0);
            if (top >= 481) break;
            rows.push_back({i, top, height, day_top, day});
            y = top + height; previous = day;
        }
        bool selected_fits = false;
        for (const auto& row : rows) if (row.index == selected && row.top + row.height <= 481) selected_fits = true;
        if (begin >= selected || selected_fits) return rows;
        ++begin;
    }
}

static const char* delivery_label(DeliveryState state) {
    switch (state) {
        case DeliveryState::Pending: return "Sending...";
        case DeliveryState::Read: return "Read";
        case DeliveryState::Failed: return "Send failed";
        default: return "Sent to Telegram";
    }
}

static void draw_delivery(float x, float y, DeliveryState state, unsigned int background, int64_t now) {
    unsigned int color = RGBA8(65,133,72,255);
    if (state == DeliveryState::Failed) {
        color = RGBA8(204,55,55,255);
        vita2d_draw_fill_circle(x+14, y+6, 7, color);
        vita2d_draw_rectangle(x+13, y+1, 2, 6, RGBA8(255,255,255,255));
        vita2d_draw_rectangle(x+13, y+9, 2, 2, RGBA8(255,255,255,255));
    } else if (state == DeliveryState::Pending) {
        vita2d_draw_fill_circle(x+14, y+6, 7, color);
        vita2d_draw_fill_circle(x+14, y+6, 5, background);
        vita2d_draw_line(x+14, y+6, x+14, y+2, color);
        bool phase = (now / 500000) % 2;
        vita2d_draw_line(x+14, y+6, x+(phase ? 18 : 11), y+(phase ? 6 : 9), color);
    } else {
        for (int tick = 0; tick < (state == DeliveryState::Read ? 2 : 1); ++tick) {
            float left = x+tick*7;
            for (int thick = 0; thick < 2; ++thick) {
                vita2d_draw_line(left, y+6+thick, left+4, y+10+thick, color);
                vita2d_draw_line(left+4, y+10+thick, left+13, y+1+thick, color);
            }
        }
    }
}

static std::vector<PostLine> post_lines(vita2d_pgf* font, const MessageItem& message) {
    std::vector<PostLine> result;
    std::string reactions;
    for (const auto& reaction : message.reactions) {
        if (!reactions.empty()) reactions += "   ";
        reactions += reaction.emoji + " " + std::to_string(reaction.count) + (reaction.chosen ? " *" : "");
    }
    if (!reactions.empty()) for (const auto& line : wrap_text(font, reactions, 840)) result.push_back({line, ""});
    if (message.delivery == DeliveryState::Failed && !message.send_error.empty())
        for (const auto& line : wrap_text(font, message.send_error, 840)) result.push_back({line, ""});
    for (const auto& line : wrap_text(font, message.text, 840)) {
        std::string target;
        int matches = 0;
        for (const auto& url : message.links) if (line.find(url) != std::string::npos) { target = url; ++matches; }
        result.push_back({line, matches == 1 ? target : ""});
    }
    // Separate rows keep multiple and hidden-text links individually tappable.
    for (const auto& url : message.links) result.push_back({fit_text(font, "Open: " + url, 840, 0.68f), url});
    return result;
}

static bool scrollable_post(const MessageItem& message) {
    return !message.voice && !message.animation && !message.sticker;
}
struct PhotoTile { int index, x, width; };
static std::vector<PhotoTile> photo_carousel(const MessageItem& message, int selected) {
    std::vector<PhotoTile> tiles;
    int x = 0, offset = 0;
    for (int i=0; i<(int)message.photos.size(); ++i) {
        const auto& photo = message.photos[i];
        float aspect = photo.width > 0 && photo.height > 0 ? (float)photo.width/photo.height : 1.0f;
        int width = std::max(100, std::min(360, (int)(170 * aspect)));
        if (i == selected) offset = x + width/2 - 480;
        tiles.push_back({i, x, width}); x += width+12;
    }
    offset = std::max(-24, std::min(offset, std::max(-24, x-936)));
    for (auto& tile : tiles) tile.x -= offset;
    return tiles;
}
static constexpr int post_photo_top = 80;
static int post_text_top(const MessageItem& message) { return message.photos.empty() ? 98 : 294; }
static void draw_post_date(vita2d_pgf* font, const MessageItem& message, int scroll,
                           unsigned int background, unsigned int color) {
    std::string label = message_day(message) + "  " + message_time(message);
    int width = rich_text_width(font, 0.55f, label.c_str()) + 28;
    rounded_rect(480-width/2, 42-scroll, width, 26, background);
    draw_text(font, 494-width/2, 60-scroll, label, color, 0.55f);
}
static int post_scroll_max(const MessageItem& message, size_t lines) {
    return std::max(0, post_text_top(message) + (int)lines*24 - 481);
}

static std::string open_web_link(const std::string& url) {
    std::string lower = url.substr(0, 8);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if ((lower.compare(0, 7, "http://") && lower != "https://") || url.find_first_of("\r\n\t ") != std::string::npos)
        return "Invalid web link";
    int rc = sceAppMgrLaunchAppByUri(0x20000, ("webmodal: " + url).c_str());
    if (rc >= 0) return "";
    char message[80];
    std::snprintf(message, sizeof(message), "Browser error: 0x%08X", rc);
    return message;
}

static void startup_screen(vita2d_pgf* font, const char* stage, int step) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    if (font) {
        // System font only: no dependency on the large Noto/emoji assets.
        vita2d_pgf_draw_text(font, 330, 225, RGBA8(35,135,210,255), 1.5f, "VitaGram " VITAGRAM_VERSION);
        vita2d_pgf_draw_text(font, 280, 285, RGBA8(39,51,66,255), 0.8f, "Starting application...");
        vita2d_pgf_draw_text(font, 280, 325, RGBA8(105,122,140,255), 0.7f, stage);
    }
    vita2d_draw_rectangle(240, 370, 480, 6, RGBA8(205,216,226,255));
    vita2d_draw_rectangle(240, 370, 480.0f*step/5, 6, RGBA8(35,135,210,255));
    vita2d_end_drawing();
    vita2d_swap_buffers();
    vita2d_wait_rendering_done();
}

int main() {
    pthread_init();
    cpu_scheduler::init();
    cpu_scheduler::Thread ui_thread(cpu_scheduler::Role::Ui);
    SceAppUtilInitParam app_util_init{};
    SceAppUtilBootParam app_util_boot{};
    sceAppUtilInit(&app_util_init, &app_util_boot);
    SceCommonDialogConfigParam dialog_config{};
    sceCommonDialogConfigParamInit(&dialog_config);
    sceCommonDialogSetConfigParam(&dialog_config);

    vita2d_init();
    vita2d_set_vblank_wait(1);
    vita2d_set_clear_color(RGBA8(238, 243, 248, 255));
    vita2d_pgf* font = vita2d_load_default_pgf();
    startup_screen(font, "1/5  Loading fonts and emoji", 1);
    if (!rich_text_init()) std::fprintf(stderr, "Noto text/emoji font initialization incomplete\n");
    startup_screen(font, "2/5  Initializing network", 2);
    VitaNet net;
    bool net_ok = net.init();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    SceCtrlData pad{};
    unsigned int old_buttons = 0;

    TelegramClient tg;
    startup_screen(font, "3/5  Restoring cached chats and media", 3);
    tg.restoreCache();
    startup_screen(font, "4/5  Starting Telegram", 4);
    if (net_ok) tg.start();

    Keyboard kb;
    AppSettings settings;
    startup_screen(font, "5/5  Loading settings", 5);
    settings.load();
    rich_text_set_scale(settings.fontScale());
    bool settings_open = false, confirm_logout = false;
    int settings_row = 0;
    std::string settings_error;
    int64_t next_cache_cleanup = sceKernelGetSystemTimeWide() + 300000000;
    TextureCache textures;
    AnimationPlayer animation;
    VoicePlayer voice;
    int chat_index = 0;
    int message_index = -1;
    int photo_index = 0;
    int post_text_scroll = 0;
    bool post_open = false;
    bool reactions_open = false;
    bool stickers_open = false;
    int sticker_index = 0, sticker_pack = 0;
    int topic_index = 0;
    int64_t square_since = 0;
    bool square_long = false;
    int reaction_index = 0;
    bool photo_open = false;
    PhotoView photo_view;
    bool running = true;
    bool touch_down = false, touch_dragged = false, touch_blocked = false;
    int touch_x = 0, touch_y = 0, touch_start_x = 0, touch_start_y = 0, touch_step_y = 0;
    unsigned int decode_cooldown = 0;
    int64_t next_network_poll = 0;
    std::string browser_error;
    int folder_scroll = 0, folder_touch_x = 0, shown_folder_index = -1;
    int32_t shown_folder_id = -1;
    bool folder_drag = false;

    while (running) {
        int64_t now = sceKernelGetSystemTimeWide();
        if (now >= next_network_poll) { net.poll(); next_network_poll = now + 1000000; }
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~old_buttons;
        unsigned int released = old_buttons & ~pad.buttons;
        old_buttons = pad.buttons;
        bool compose_context = tg.currentChatId() && !tg.topicsOpen() && !post_open && !photo_open && !settings_open &&
            !reactions_open && !stickers_open && !kb.active() && tg.inputKind() == InputKind::None;
        if (compose_context) {
            if (pressed & SCE_CTRL_SQUARE) { square_since = now; square_long = false; }
            pressed &= ~SCE_CTRL_SQUARE;
            if ((pad.buttons & SCE_CTRL_SQUARE) && square_since && now-square_since >= 550000) {
                square_long = true; square_since = 0;
                stickers_open = true; sticker_index = sticker_pack = 0; tg.openStickers();
            }
            if (released & SCE_CTRL_SQUARE) {
                if (square_since && !square_long) pressed |= SCE_CTRL_SQUARE;
                square_since = 0;
            }
        } else square_since = 0;

        int64_t selected_chat = chat_index >= 0 && chat_index < (int)tg.chats().size() ? tg.chats()[chat_index].id : 0;
        int64_t selected_id = message_index >= 0 && message_index < (int)tg.messages().size() ? tg.messages()[message_index].id : 0;
        int64_t selected_album = message_index >= 0 && message_index < (int)tg.messages().size() ? tg.messages()[message_index].media_album_id : 0;
        int64_t previous_last = tg.messages().empty() ? 0 : tg.messages().back().id;
        bool was_at_bottom = message_index < 0 || message_index + 1 == (int)tg.messages().size();
        bool was_cleaning = tg.cacheMaintenance();
        bool was_ready = tg.ready();
        tg.pump();
        if (!tg.currentChatId()) reactions_open = false;
        if (!tg.currentChatId() || !tg.ready()) stickers_open = false;
        if ((was_cleaning && !tg.cacheMaintenance()) || (was_ready && !tg.ready())) {
            vita2d_wait_rendering_done(); textures.clear();
        }
        cpu_scheduler::tick();
        if (selected_chat) {
            for (size_t i = 0; i < tg.chats().size(); ++i)
                if (tg.chats()[i].id == selected_chat) { chat_index = (int)i; break; }
        }
        chat_index = std::max(0, std::min(chat_index, (int)tg.chats().size() - 1));
        if (tg.takeChatTopRequest() && !tg.currentChatId()) chat_index = 0;
        if (selected_id) {
            const auto& updated = tg.messages();
            for (size_t i = 0; i < updated.size(); ++i)
                if (updated[i].id == selected_id || (selected_album && updated[i].media_album_id == selected_album)) { message_index = (int)i; break; }
        }
        if (was_at_bottom && !post_open && !photo_open && !tg.messages().empty() &&
            tg.messages().back().outgoing && tg.messages().back().id != previous_last)
            message_index = (int)tg.messages().size() - 1;

        const bool folder_tabs = !tg.folders().empty();
        const int chat_rows = folder_tabs ? 8 : 9;
        const int chat_top = folder_tabs ? 90 : 48;
        const int chat_row_height = (481-chat_top)/chat_rows;
        const int folder_max_scroll = std::max(0, (int)tg.folders().size() * 180 - 920);
        folder_scroll = std::max(0, std::min(folder_scroll, folder_max_scroll));
        SceTouchData touch{};
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        photo_view.updateMotion(photo_open, sceKernelGetProcessTimeWide(), touch.reportNum != 0);
        bool photo_gesture = photo_open && photo_view.touch(touch);
        bool touching = touch.reportNum > 0;
        bool tap = false;
        if (touching) {
            touch_x = touch.report[0].x / 2;
            touch_y = touch.report[0].y / 2;
            if (!touch_down) {
                touch_start_x = touch_x; touch_start_y = touch_step_y = touch_y;
                touch_dragged = false;
                touch_blocked = kb.active() || tg.inputKind() != InputKind::None;
                folder_drag = !stickers_open && !settings_open && folder_tabs && !tg.currentChatId() && touch_start_y >= 38 && touch_start_y < 82;
                folder_touch_x = touch_x;
            }
            if (std::abs(touch_y - touch_start_y) > 14 || std::abs(touch_x - touch_start_x) > 14) touch_dragged = true;
            if (folder_drag && !touch_blocked && touch_dragged) {
                folder_scroll = std::max(0, std::min(folder_max_scroll, folder_scroll + folder_touch_x - touch_x));
            }
            folder_touch_x = touch_x;
            if (!folder_drag && !touch_blocked && touch_start_y >= 38 && touch_start_y < 499 && !photo_open &&
                std::abs(touch_y - touch_start_y) >= std::abs(touch_x - touch_start_x) &&
                std::abs(touch_y - touch_step_y) >= 32) {
                pressed |= touch_y < touch_step_y ? SCE_CTRL_DOWN : SCE_CTRL_UP;
                touch_step_y = touch_y;
            }
        } else if (touch_down && !touch_blocked && !photo_gesture) {
            tap = !touch_dragged;
            if ((photo_open || post_open) && std::abs(touch_x - touch_start_x) > 45 &&
                std::abs(touch_x - touch_start_x) > std::abs(touch_y - touch_start_y))
                pressed |= touch_x < touch_start_x ? SCE_CTRL_RIGHT : SCE_CTRL_LEFT;
        }
        touch_down = touching;
        if (touching || pressed) decode_cooldown = 8;
        else if (decode_cooldown) --decode_cooldown;
        textures.beginFrame(decode_cooldown != 0 || kb.active());

        if (tap && !kb.active() && tg.inputKind() == InputKind::None) {
            if (tg.topicsOpen()) {
                if (touch_y >= 500) pressed |= touch_x >= 650 ? SCE_CTRL_CIRCLE : SCE_CTRL_TRIANGLE;
                else if (touch_y >= 70 && touch_y < 470) {
                    int index = std::max(0, topic_index-7)+(touch_y-70)/50;
                    if (index < (int)tg.topics().size()) { topic_index = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (stickers_open) {
                if (touch_y >= 500) pressed |= SCE_CTRL_CIRCLE;
                else if (touch_y >= 42 && touch_y < 98) pressed |= touch_x < 480 ? SCE_CTRL_LTRIGGER : SCE_CTRL_RTRIGGER;
                else if (touch_x >= 30 && touch_x < 930 && touch_y >= 130 && touch_y < 460) {
                    int index = (sticker_index/18)*18 + (touch_y-130)/110*6 + (touch_x-30)/150;
                    if (index < (int)tg.stickers().size()) { sticker_index = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (reactions_open) {
                if (touch_y >= 500) pressed |= SCE_CTRL_CIRCLE;
                else if (touch_x >= 32 && touch_x < 928 && touch_y >= 130 && touch_y < 418) {
                    int index = (reaction_index/32)*32 + (touch_y-130)/72*8 + (touch_x-32)/112;
                    if (index < (int)tg.reactionChoices().size()) { reaction_index = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (settings_open) {
                if (touch_y >= 500) pressed |= SCE_CTRL_CIRCLE;
                else if (touch_y >= 104 && touch_y < 404) {
                    settings_row = (touch_y - 104) / 60; pressed |= SCE_CTRL_CROSS;
                }
            } else if (!tg.currentChatId() && tg.canBrowse() && folder_tabs && touch_y >= 38 && touch_y < 82 && touch_x >= 20 && touch_x < 940) {
                int index = (touch_x - 20 + folder_scroll) / 180;
                if (index < (int)tg.folders().size()) { tg.selectFolder(tg.folders()[index].id); chat_index = 0; }
            } else if (!tg.currentChatId() && tg.canBrowse() && touch_y >= 481 && touch_y < 500) {
                tg.loadMoreChats();
            } else if (touch_y >= 500) {
                if (tg.currentChatId()) pressed |= touch_x >= 650 ? SCE_CTRL_CIRCLE :
                    (post_open || photo_open || touch_x < 280 ? SCE_CTRL_CROSS : SCE_CTRL_SQUARE);
                else pressed |= touch_x >= 650 ? SCE_CTRL_TRIANGLE : SCE_CTRL_CROSS;
            } else if (tg.currentChatId() && !post_open && !photo_open && touch_y >= 40 && touch_y < 481) {
                for (const auto& row : history_layout(tg.messages(), message_index, tg.currentChatIsGroup()))
                    if (touch_y >= row.top && touch_y < row.top + row.height - 4) {
                        message_index = row.index; pressed |= SCE_CTRL_CROSS; break;
                    }
            } else if (post_open && !photo_open && message_index >= 0 && message_index < (int)tg.messages().size()) {
                const auto& post = tg.messages()[message_index];
                if (scrollable_post(post)) {
                    if (touch_y >= 40 && touch_y < 481) {
                        if (!post.photos.empty() && touch_y >= post_photo_top-post_text_scroll && touch_y < post_photo_top+170-post_text_scroll) {
                            for (const auto& tile : photo_carousel(post, photo_index))
                                if (touch_x >= tile.x && touch_x < tile.x+tile.width) {
                                    photo_index = tile.index; pressed |= SCE_CTRL_CROSS; break;
                                }
                        } else {
                            auto lines = post_lines(font, post);
                            int relative = touch_y - (post_text_top(post)-19-post_text_scroll);
                            int line = relative / 24;
                            if (relative >= 0 && line < (int)lines.size() && !lines[line].url.empty())
                                browser_error = open_web_link(lines[line].url);
                        }
                    }
                } else {
                int count = (int)tg.messages()[message_index].photos.size();
                int columns = count > 1 ? 2 : 1;
                int height = count > 2 ? 120 : 210;
                int base = (photo_index / 6) * 6;
                int x0 = columns == 2 ? 38 : 160;
                int col = (touch_x - x0) / 444;
                int row = (touch_y - 90) / (height + 8);
                int index = base + row * columns + col;
                int text_y = (tg.messages()[message_index].sticker || tg.messages()[message_index].animation || tg.messages()[message_index].voice) ? 330 : (count == 0 ? 110 : (count > 2 ? 470 : 330));
                if ((tg.messages()[message_index].voice || tg.messages()[message_index].video) && touch_x >= 160 && touch_x < 800 && touch_y >= 90 && touch_y < 300)
                    pressed |= SCE_CTRL_CROSS;
                if (touch_x >= 38 && touch_x < 930 && touch_y >= text_y - 19 && touch_y < 481) {
                    auto lines = post_lines(font, tg.messages()[message_index]);
                    int line = post_text_scroll + (touch_y - (text_y - 19)) / 24;
                    if (line >= 0 && line < (int)lines.size() && !lines[line].url.empty())
                        browser_error = open_web_link(lines[line].url);
                }
                if (touch_x >= x0 && touch_x < (columns == 2 ? 912 : 800) && touch_y >= 90 && touch_y < text_y - 19 &&
                    row >= 0 && row < (count > 2 ? 3 : 1) && col < columns && index < count) {
                    photo_index = index; pressed |= SCE_CTRL_CROSS;
                }
                }
            } else if (!tg.currentChatId() && tg.canBrowse() && touch_y >= chat_top && touch_y < chat_top + chat_rows * chat_row_height) {
                int index = std::max(0, chat_index - chat_rows + 1) + (touch_y - chat_top) / chat_row_height;
                if (index < (int)tg.chats().size()) { chat_index = index; pressed |= SCE_CTRL_CROSS; }
            }
        }

        if (tg.currentChatId() != 0 && !tg.messages().empty()) {
            if (message_index < 0) message_index = (int)tg.messages().size() - 1;
            message_index = std::max(0, std::min(message_index, (int)tg.messages().size() - 1));
            photo_index = std::max(0, std::min(photo_index, (int)tg.messages()[message_index].photos.size() - 1));
        }

        if (tg.inputKind() != InputKind::None && !kb.active() && !kb.submitted() && !kb.cancelled()) {
            kb.open(tg.inputPrompt(), tg.inputSecret());
        }

        bool toggle_voice = false;
        bool toggle_video = false;
        if ((pressed & SCE_CTRL_START) && !(pad.buttons & SCE_CTRL_SELECT) && !kb.active() && !tg.currentChatId()) {
            settings_open = !settings_open; confirm_logout = false; pressed = 0;
        }
        if (kb.active()) {
            kb.handle(pressed);
        } else if (kb.submitted()) {
            std::string v = kb.text();
            kb.close();
            tg.submitInput(v);
        } else if (kb.cancelled()) {
            kb.close();
            tg.cancelInput();
        } else if (tg.topicsOpen()) {
            if (pressed & SCE_CTRL_CIRCLE) tg.leaveChat();
            else {
                if (pressed & SCE_CTRL_UP) --topic_index;
                if (pressed & SCE_CTRL_DOWN) {
                    ++topic_index;
                    if (topic_index >= (int)tg.topics().size()-2) tg.loadTopics();
                }
                if (pressed & SCE_CTRL_TRIANGLE) tg.loadTopics(tg.topics().empty());
                topic_index = std::max(0, std::min(std::max(0, (int)tg.topics().size()-1), topic_index));
                if (pressed & SCE_CTRL_CROSS) { tg.openTopic(topic_index); message_index = -1; }
            }
        } else if (stickers_open) {
            if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_SQUARE)) stickers_open = false;
            int packs = (int)tg.stickerPacks().size(), previous_pack = sticker_pack;
            if (packs && (pressed & SCE_CTRL_LTRIGGER)) sticker_pack = (sticker_pack+packs-1)%packs;
            if (packs && (pressed & SCE_CTRL_RTRIGGER)) sticker_pack = (sticker_pack+1)%packs;
            if (previous_pack != sticker_pack) { sticker_index = 0; tg.selectStickerPack(sticker_pack); }
            int count = (int)tg.stickers().size();
            if (pressed & SCE_CTRL_LEFT) --sticker_index;
            if (pressed & SCE_CTRL_RIGHT) ++sticker_index;
            if (pressed & SCE_CTRL_UP) sticker_index -= 6;
            if (pressed & SCE_CTRL_DOWN) sticker_index += 6;
            sticker_index = std::max(0, std::min(std::max(0, count-1), sticker_index));
            if (stickers_open && (pressed & SCE_CTRL_CROSS) && tg.sendSticker(sticker_index)) stickers_open = false;
        } else if (reactions_open) {
            int count = (int)tg.reactionChoices().size();
            if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_SQUARE)) reactions_open = false;
            if (count) {
                if (pressed & SCE_CTRL_LEFT) --reaction_index;
                if (pressed & SCE_CTRL_RIGHT) ++reaction_index;
                if (pressed & SCE_CTRL_UP) reaction_index -= 8;
                if (pressed & SCE_CTRL_DOWN) reaction_index += 8;
                reaction_index = std::max(0, std::min(count-1, reaction_index));
                if ((pressed & SCE_CTRL_CROSS) && message_index >= 0 && message_index < (int)tg.messages().size())
                    tg.toggleReaction(tg.messages()[message_index].id, tg.reactionChoices()[reaction_index]);
            }
        } else if (settings_open) {
            if (pressed & SCE_CTRL_CIRCLE) { if (confirm_logout) confirm_logout = false; else settings_open = false; }
            if (pressed & SCE_CTRL_UP) { settings_row = (settings_row+4)%5; confirm_logout = false; }
            if (pressed & SCE_CTRL_DOWN) { settings_row = (settings_row+1)%5; confirm_logout = false; }
            if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) {
                int direction = (pressed & SCE_CTRL_LEFT) ? -1 : 1;
                if (settings_row < 4) {
                    confirm_logout = false;
                    int* values[] = {&settings.retention, &settings.capacity, &settings.theme, &settings.font};
                    const int counts[] = {5, 6, 2, 3};
                    *values[settings_row] = (*values[settings_row] + counts[settings_row] + direction) % counts[settings_row];
                    rich_text_set_scale(settings.fontScale());
                    settings_error = settings.save() ? "" : "Не удалось сохранить настройки";
                    next_cache_cleanup = now + 300000000;
                } else if (pressed & SCE_CTRL_CROSS) {
                    if (!tg.ready()) settings_error = "Для выхода дождитесь подключения";
                    else if (confirm_logout) { tg.logout(); confirm_logout = false; settings_open = false; }
                    else confirm_logout = true;
                }
            }
        } else if (tg.currentChatId() != 0) {
            const auto& messages = tg.messages();
            if (photo_open) {
                if (pressed & SCE_CTRL_CIRCLE) photo_open = false;
                if (pressed & SCE_CTRL_TRIANGLE) photo_view.reset();
                if (!messages.empty() && message_index >= 0) {
                    int previous_photo = photo_index;
                    int count = (int)messages[message_index].photos.size();
                    if ((pressed & SCE_CTRL_LEFT) && count) photo_index = (photo_index + count - 1) % count;
                    if ((pressed & SCE_CTRL_RIGHT) && count) photo_index = (photo_index + 1) % count;
                    if (photo_index != previous_photo) photo_view.reset();
                    if (count) tg.ensureFullPhoto((size_t)message_index, (size_t)photo_index);
                }
            } else if (post_open) {
                if (pressed & SCE_CTRL_CIRCLE) post_open = false;
                if ((pressed & SCE_CTRL_SQUARE) && message_index >= 0 && message_index < (int)messages.size()) {
                    reactions_open = true; reaction_index = 0;
                    tg.loadReactions(messages[message_index].id); pressed = 0;
                }
                if (post_open && !messages.empty() && message_index >= 0) {
                    int next = message_index;
                    if (pressed & SCE_CTRL_LEFT) next = std::max(0, message_index-1);
                    else if (pressed & SCE_CTRL_RIGHT) next = std::min((int)messages.size()-1, message_index+1);
                    if (next != message_index) {
                        message_index = next;
                        photo_index = 0;
                        post_text_scroll = 0;
                        browser_error.clear();
                        // Stop the previous post even when adjacent posts share a media path.
                        tg.setAnimationTarget(-1);
                        tg.setVoiceTarget(-1);
                        animation.setSource("", false, false);
                        voice.setSource("", false, false);
                        pressed &= ~(SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE | SCE_CTRL_UP | SCE_CTRL_DOWN);
                    }
                    if ((pressed & SCE_CTRL_LEFT) && message_index <= 2) tg.loadOlder();
                }
                if (!messages.empty() && message_index >= 0) {
                    int count = (int)messages[message_index].photos.size();
                    if ((pressed & SCE_CTRL_LTRIGGER) && count) photo_index = (photo_index + count - 1) % count;
                    if ((pressed & SCE_CTRL_RTRIGGER) && count) photo_index = (photo_index + 1) % count;
                    auto lines = post_lines(font, messages[message_index]);
                    if ((pressed & SCE_CTRL_CROSS) && messages[message_index].voice) toggle_voice = true;
                    if ((pressed & SCE_CTRL_CROSS) && messages[message_index].video) toggle_video = true;
                    if (scrollable_post(messages[message_index])) {
                        if (pressed & SCE_CTRL_UP) post_text_scroll -= 24;
                        if (pressed & SCE_CTRL_DOWN) post_text_scroll += 24;
                        post_text_scroll = std::max(0, std::min(post_text_scroll, post_scroll_max(messages[message_index], lines.size())));
                    } else {
                    if ((pressed & SCE_CTRL_UP) && post_text_scroll > 0) post_text_scroll--;
                    if ((pressed & SCE_CTRL_DOWN) && post_text_scroll + 1 < (int)lines.size()) post_text_scroll++;
                    }
                    int link_line = scrollable_post(messages[message_index]) ? std::max(0, (post_text_scroll-post_text_top(messages[message_index])+62)/24) : post_text_scroll;
                    if ((pressed & SCE_CTRL_TRIANGLE) && link_line < (int)lines.size() && !lines[link_line].url.empty())
                        browser_error = open_web_link(lines[link_line].url);
                    if ((pressed & SCE_CTRL_CROSS) && count) {
                        tg.ensureFullPhoto((size_t)message_index, (size_t)photo_index);
                        photo_open = true;
                        photo_view.reset();
                    }
                }
            } else {
                if (pressed & SCE_CTRL_CIRCLE) { if (tg.inTopic()) tg.backToTopics(); else tg.leaveChat(); pressed = 0; }
                if (pressed & SCE_CTRL_SQUARE) tg.requestCompose();
                if (pressed & SCE_CTRL_TRIANGLE) tg.refreshHistory();
                if (pressed & SCE_CTRL_UP) {
                    if (message_index > 0) message_index--;
                    if (message_index <= 2) tg.loadOlder();
                }
                if ((pressed & SCE_CTRL_DOWN) && message_index + 1 < (int)messages.size()) message_index++;
                if ((pressed & SCE_CTRL_CROSS) && !messages.empty() && message_index >= 0) {
                    post_open = true;
                    browser_error.clear();
                    photo_index = 0;
                    post_text_scroll = 0;
                }
            }
        } else if (tg.canBrowse()) {
            if (folder_tabs && ((pressed & SCE_CTRL_LTRIGGER) || (pressed & SCE_CTRL_RTRIGGER))) {
                int current = 0;
                for (int i = 0; i < (int)tg.folders().size(); ++i) if (tg.folders()[i].id == tg.selectedFolder()) current = i;
                int direction = (pressed & SCE_CTRL_RTRIGGER) ? 1 : -1;
                int next = (current + direction + (int)tg.folders().size()) % (int)tg.folders().size();
                tg.selectFolder(tg.folders()[next].id); chat_index = 0;
                pressed &= ~SCE_CTRL_CROSS; // Never open a stale selection during a tab switch.
            }
            const auto& chats = tg.chats();
            if (!chats.empty()) {
                if (pressed & SCE_CTRL_UP) chat_index--;
                if (pressed & SCE_CTRL_DOWN) {
                    if (chat_index + 1 < (int)chats.size()) chat_index++;
                    if (chat_index >= (int)chats.size() - 2) tg.loadMoreChats();
                }
                chat_index = std::max(0, std::min(chat_index, (int)chats.size() - 1));
                if (pressed & SCE_CTRL_CROSS) {
                    tg.openChat(chats[chat_index].id, chats[chat_index].title);
                    topic_index = 0;
                    message_index = -1;
                    post_open = photo_open = false;
                }
            }
            if (pressed & SCE_CTRL_TRIANGLE) tg.refreshChats();
        }

        if ((pressed & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT) && !kb.active()) {
            tg.close();
            running = false;
        }

        int animation_index = post_open && !photo_open && !kb.active() && running &&
            message_index >= 0 && message_index < (int)tg.messages().size() && tg.messages()[message_index].animation ? message_index : -1;
        tg.setAnimationTarget(animation_index);
        const MessageItem* animation_message = animation_index >= 0 ? &tg.messages()[animation_index] : nullptr;
        animation.setSource(animation_message && animation_message->animation_error.empty() ? animation_message->animation_path : "",
            animation_message && animation_message->video, animation_message && animation_message->video_note);
        if (toggle_video && animation_message) animation.toggle();
        animation.upload();
        int voice_index = post_open && !photo_open && !kb.active() && running && tg.currentChatId() &&
            message_index >= 0 && message_index < (int)tg.messages().size() && tg.messages()[message_index].voice ? message_index : -1;
        tg.setVoiceTarget(voice_index);
        const MessageItem* voice_message = voice_index >= 0 ? &tg.messages()[voice_index] : nullptr;
        voice.setSource(voice_message && voice_message->voice_error.empty() ? voice_message->voice_path : "", false,
            voice_message && voice_message->audio);
        if (toggle_voice && voice_message) voice.toggle();

        if (pressed || touching || tg.currentChatId() || settings_open || !tg.ready())
            next_cache_cleanup = now + 300000000;
        if (tg.ready() && !tg.currentChatId() && !settings_open && now >= next_cache_cleanup) {
            tg.optimizeCache(AppSettings::days(settings.retention), AppSettings::megabytes(settings.capacity));
            next_cache_cleanup = now + 300000000;
        }
        bool dark = settings.theme == 1;
        const unsigned int white = dark ? RGBA8(31,45,59,255) : RGBA8(255,255,255,255);
        const unsigned int ink = dark ? RGBA8(231,238,245,255) : RGBA8(39,51,66,255);
        const unsigned int muted = dark ? RGBA8(163,183,200,255) : RGBA8(105,122,140,255);
        const unsigned int accent = RGBA8(35,135,210,255);
        const unsigned int selected = dark ? RGBA8(46,72,94,255) : RGBA8(207,232,249,255);
        const unsigned int outgoing = RGBA8(20,143,82,255);
        const unsigned int separator = dark ? RGBA8(53,70,86,255) : RGBA8(205,216,226,255);
        vita2d_set_clear_color(dark ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255));

        vita2d_start_drawing();
        vita2d_clear_screen();

        vita2d_draw_rectangle(0, 0, 960, 36, dark ? white : accent);
        int title_x = 20;
        if (tg.currentChatId()) {
            if (!decode_cooldown) for (size_t i=0; i<tg.chats().size(); ++i)
                if (tg.chats()[i].id == tg.currentChatId()) { tg.ensureChatMedia(i); break; }
            auto avatar = textures.get(tg.currentChatAvatar());
            if (avatar) draw_texture_fit(avatar, 12, 4, 28, 28);
            else rounded_rect(12, 4, 28, 28, selected);
            title_x = 50;
        }
        const unsigned int header_text = RGBA8(240,247,255,255);
        std::string heading = settings_open ? "Настройки" : tg.currentChatId() ? tg.currentChatTitle() : "VitaGram " VITAGRAM_VERSION;
        std::string presence = settings_open ? "" : tg.currentUserStatus();
        int status_width = presence.empty() ? 280 : 390;
        draw_text(font, title_x, 25, fit_text(font, heading, 920-status_width-title_x, 0.65f), header_text, 0.65f);
        std::string network = fit_text(font, presence.empty() ? (net_ok ? net.status() : net.error()) : presence, status_width, 0.52f);
        draw_text(font, 940-rich_text_width(font, 0.52f, network.c_str()), 25, network, header_text, 0.52f);
        std::string update_notice = tg.updateNotice();

        vita2d_draw_rectangle(0, 500, 960, 44, white);
        vita2d_draw_rectangle(0, 499, 960, 1, separator);

        if (settings_open) {
            const char* labels[] = {"Хранить кэш", "Объём кэша медиа", "Тема", "Размер шрифта", "Выйти из аккаунта"};
            std::string values[] = {
                settings.retention == 4 ? "Всегда" : std::to_string(AppSettings::days(settings.retention)) + " дн.",
                settings.capacity == 5 ? "Без лимита" : std::to_string(AppSettings::megabytes(settings.capacity)) + " МБ",
                dark ? "Тёмная" : "Светлая", std::to_string(90+settings.font*10) + "%", confirm_logout ? "X: подтвердить выход" : ""};
            for (int row=0; row<5; ++row) {
                rounded_rect(24, 104+row*60, 912, 54, row == settings_row ? selected : white);
                draw_text(font, 40, 138+row*60, labels[row], ink, 0.7f);
                draw_text(font, 520, 138+row*60, values[row], ink, 0.65f);
            }
            draw_text(font, 30, 437, "Кэш медиа очищается в главном меню. Сообщения сохраняются.", muted, 0.55f);
            draw_text(font, 30, 465, settings_error, ink, 0.6f);
            draw_text(font, 28, 528, "D-pad: выбор / изменение    X Изменить    O Назад", ink, 0.6f);
        } else if (!net_ok) {
            draw_text(font, 30, 115, "Network initialization failed.", ink, 0.85f);
        } else if (!tg.lastError().empty() && !tg.ready()) {
            draw_text(font, 30, 115, clip(tg.lastError(), 100), ink, 0.75f);
        }

        // Settings own the content area and footer; never draw a second screen over them.
        if (!settings_open && !stickers_open && !tg.topicsOpen()) {
        if (tg.currentChatId() != 0) {
            const auto& msgs = tg.messages();
            if (photo_open && message_index >= 0 && message_index < (int)msgs.size() &&
                photo_index < (int)msgs[message_index].photos.size()) {
                const PhotoItem& photo = msgs[message_index].photos[photo_index];
                vita2d_draw_rectangle(0, 38, 960, 468, RGBA8(8,12,18,255));
                vita2d_texture* image = textures.get(photo.full_path.empty() ? photo.thumbnail_path : photo.full_path);
                if (image) photo_view.draw(image);
                else draw_text(font, 390, 290, "Loading full image...", header_text, 0.75f);
                if (photo.full_path.empty() && image) draw_text(font, 30, 480, "Loading larger photo...", header_text, 0.65f);
                char counter[32];
                std::snprintf(counter, sizeof(counter), "%d / %d", photo_index + 1, (int)msgs[message_index].photos.size());
                draw_text(font, 440, 60, counter, header_text, 0.65f);
                draw_text(font, 28, 528, "D-pad: фото   Pinch: zoom   Triangle: сброс", ink, 0.55f);
                draw_text(font, 700, 528, "O  Back", ink, 0.65f);
            } else if (post_open && message_index >= 0 && message_index < (int)msgs.size() && scrollable_post(msgs[message_index])) {
                const auto& message = msgs[message_index];
                auto lines = post_lines(font, message);
                post_text_scroll = std::max(0, std::min(post_text_scroll, post_scroll_max(message, lines.size())));
                vita2d_enable_clipping();
                vita2d_set_clip_rectangle(0, 38, 960, 481);
                draw_post_date(font, message, post_text_scroll, selected, muted);
                int photo_y = post_photo_top-post_text_scroll;
                if (photo_y+170 > 38) {
                    for (const auto& tile : photo_carousel(message, photo_index)) {
                        if (tile.x >= 960 || tile.x+tile.width <= 0) continue;
                        if (!decode_cooldown) tg.ensurePhotoThumbnail(message_index, tile.index);
                        rounded_rect(tile.x-2, photo_y-2, tile.width+4, 174, tile.index == photo_index ? accent : separator);
                        auto texture = textures.get(message.photos[tile.index].thumbnail_path);
                        if (texture) {
                            float w = vita2d_texture_get_width(texture), h = vita2d_texture_get_height(texture);
                            float scale = std::min(tile.width/w, 170.0f/h);
                            draw_texture_fit(texture, tile.x+(tile.width-w*scale)/2, photo_y+(170-h*scale)/2, tile.width, 170);
                        } else draw_text(font, tile.x+12, photo_y+90, "…", muted, 0.65f);
                    }
                }
                if (!message.photos.empty()) {
                    std::string counter = std::to_string(photo_index+1)+" / "+std::to_string(message.photos.size());
                    int width = rich_text_width(font, 0.55f, counter.c_str());
                    int y = 270-post_text_scroll;
                    draw_text(font, 480-width/2, y, counter, muted, 0.55f);
                    // Draw navigation chevrons without relying on missing font glyphs.
                    int left = 460-width/2, right = 500+width/2;
                    vita2d_draw_line(left+5, y-13, left, y-8, muted);
                    vita2d_draw_line(left, y-8, left+5, y-3, muted);
                    vita2d_draw_line(right-5, y-13, right, y-8, muted);
                    vita2d_draw_line(right, y-8, right-5, y-3, muted);
                }
                int text_y = post_text_top(message)-post_text_scroll;
                for (int line=0; line<(int)lines.size(); ++line) {
                    int baseline = text_y+line*24;
                    if (baseline < 38) continue;
                    if (baseline-22 >= 481) break;
                    draw_text(font, 38, baseline, lines[line].text, lines[line].url.empty() ? ink : accent, 0.68f);
                }
                vita2d_disable_clipping();
                int maximum = post_scroll_max(message, lines.size());
                if (maximum > 0) {
                    float thumb = std::max(28.0f, 443.0f*443/(443+maximum));
                    vita2d_draw_rectangle(953, 38+(443-thumb)*post_text_scroll/maximum, 3, thumb, accent);
                }
                draw_text(font, 28, 528, "D-pad: пост   L/R: фото", ink, 0.6f);
                draw_text(font, 390, 528, "X Открыть фото", ink, 0.65f);
                draw_text(font, 750, 528, "O Назад", ink, 0.65f);
            } else if (post_open && message_index >= 0 && message_index < (int)msgs.size()) {
                const MessageItem& message = msgs[message_index];
                draw_post_date(font, message, 0, selected, muted);
                int count = (int)message.photos.size();
                int columns = count > 1 ? 2 : 1;
                int cell_w = columns == 2 ? 430 : 640;
                int cell_h = count > 2 ? 120 : 210;
                int first = (photo_index / 6) * 6;
                int shown = std::min(count, first + 6);
                for (int i = first; i < shown; ++i) {
                    if (!decode_cooldown) tg.ensurePhotoThumbnail((size_t)message_index, (size_t)i);
                    int col = (i - first) % columns, row = (i - first) / columns;
                    float x = columns == 2 ? 38.0f + col * 444.0f : 160.0f;
                    float y = 90.0f + row * (cell_h + 8.0f);
                    vita2d_draw_rectangle(x - 4, y - 4, (float)cell_w + 8, (float)cell_h + 8,
                        i == photo_index ? accent : white);
                    draw_texture_fit(textures.get(message.photos[i].thumbnail_path), x, y, (float)cell_w, (float)cell_h);
                }
                int text_y = (message.sticker || message.animation || message.voice) ? 330 : (count == 0 ? 110 : (count > 2 ? 470 : 330));
                if (message.voice) {
                    vita2d_draw_rectangle(160, 90, 640, 210, selected);
                    std::string state = message.voice_error;
                    if (state.empty() && message.voice_path.empty()) {
                        if (!message.voice_file_id) state = "Audio file unavailable";
                        else if (message.voice_size > 0) state = "Loading audio: " + std::to_string(std::min<int64_t>(99, message.voice_downloaded * 100 / message.voice_size)) + "%";
                        else state = "Loading audio: " + std::to_string(message.voice_downloaded / 1024) + " KiB";
                    } else if (state.empty()) state = voice.status();
                    draw_text(font, 184, 130, fit_text(font, message.audio ? message.audio_title : "Voice message", 590, 0.8f), ink, 0.8f);
                    if (message.audio) draw_text(font, 184, 157, fit_text(font, message.audio_performer, 590, 0.6f), muted, 0.6f);
                    draw_text(font, 184, 190, fit_text(font, state, 590, 0.7f), accent, 0.7f);
                    int elapsed = voice.seconds(), duration = message.voice_duration;
                    char time[64]; std::snprintf(time, sizeof(time), "%d:%02d / %d:%02d", elapsed / 60, elapsed % 60, duration / 60, duration % 60);
                    draw_text(font, 184, 237, time, muted, 0.7f);
                    vita2d_draw_rectangle(184, 263, 592, 6, separator);
                    if (duration > 0) vita2d_draw_rectangle(184, 263, 592.0f * std::min(elapsed, duration) / duration, 6, accent);
                }
                if (message.animation) {
                    if (!decode_cooldown) tg.ensureMessageMedia((size_t)message_index);
                    vita2d_draw_rectangle(160, 86, 640, 214, message.sticker ? white : RGBA8(8,12,18,255));
                    vita2d_texture* picture = animation.texture();
                    if (!picture) picture = textures.get(message.media_path);
                    if (picture) {
                        float w = vita2d_texture_get_width(picture), h = vita2d_texture_get_height(picture);
                        float scale = std::min(640.0f / w, 210.0f / h);
                        draw_texture_fit(picture, 160 + (640-w*scale)/2, 88 + (210-h*scale)/2, 640, 210);
                    }
                    std::string state = message.animation_error;
                    if (state.empty() && message.animation_path.empty()) {
                        if (message.animation_file_id <= 0) state = "Media file unavailable";
                        else if (message.animation_size > 0)
                            state = std::string(message.video ? "Loading video: " : "Loading GIF: ") + std::to_string(std::min<int64_t>(99, message.animation_downloaded * 100 / message.animation_size)) + "%";
                        else state = std::string(message.video ? "Loading video: " : "Loading GIF: ") + std::to_string(message.animation_downloaded / 1024) + " KiB";
                    } else if (state.empty()) state = animation.status();
                    if (message.video && !message.animation_path.empty()) {
                        int elapsed = animation.seconds(), duration = message.video_duration;
                        char timer[64]; std::snprintf(timer, sizeof(timer), "%d:%02d / %d:%02d  ", elapsed/60, elapsed%60, duration/60, duration%60);
                        state = timer + state;
                        vita2d_draw_rectangle(160, 297, 640, 3, separator);
                        if (duration > 0) vita2d_draw_rectangle(160, 297, 640.0f * std::min(elapsed, duration) / duration, 3, accent);
                    }
                    draw_text(font, 164, 313, fit_text(font, state, 760, 0.55f), muted, 0.55f);
                }
                if (message.sticker && !message.animation) {
                    if (!decode_cooldown) tg.ensureMessageMedia((size_t)message_index);
                    vita2d_texture* sticker = textures.get(message.media_path);
                    if (sticker) draw_texture_fit(sticker, 375, 90, 210, 210);
                    else draw_text(font, 300, 190, message.media_file_id ? "Loading sticker..." : "No sticker preview", muted, 0.7f);
                }
                if (count > 6) {
                    char more[32]; std::snprintf(more, sizeof(more), "+%d photos", count - 6);
                    draw_text(font, 785, 480, more, muted, 0.65f);
                }
                auto text_lines = post_lines(font, message);
                int available_lines = std::max(1, (490 - text_y) / 24);
                post_text_scroll = std::max(0, std::min(post_text_scroll,
                    std::max(0, (int)text_lines.size() - available_lines)));
                for (int line = 0; line < available_lines && post_text_scroll + line < (int)text_lines.size(); ++line)
                    draw_text(font, 38, text_y + line * 24, text_lines[post_text_scroll + line].text,
                        text_lines[post_text_scroll + line].url.empty() ? (message.outgoing ? outgoing : ink) : accent, 0.68f);
                draw_text(font, 28, 528, "D-pad: пост   L/R: фото", ink, 0.6f);
                draw_text(font, 390, 528, (message.voice || message.video) ? "X Play / Pause" : "Tap link / X photo", ink, 0.65f);
                draw_text(font, 750, 528, "O  Back", ink, 0.65f);
            } else {
                auto history_rows = history_layout(msgs, message_index, tg.currentChatIsGroup());
                vita2d_enable_clipping();
                vita2d_set_clip_rectangle(0, 38, 960, 481);
                for (const auto& entry : history_rows) {
                    int i = entry.index;
                    if (!decode_cooldown) tg.ensureMessageMedia((size_t)i);
                    if (entry.day_top >= 0) {
                        int width = rich_text_width(font, 0.55f, entry.day.c_str()) + 24;
                        rounded_rect(480-width/2, entry.day_top, width, 22, selected);
                        draw_text(font, 492-width/2, entry.day_top+16, entry.day, muted, 0.55f);
                    }
                    int y = entry.top + 4;
                    const MessageItem& m = msgs[i];
                    bool show_sender = tg.currentChatIsGroup() && !m.outgoing;
                    std::string sender = show_sender ? fit_text(font, tg.senderName(m), 680, 0.55f) : "";
                    bool selected_message = i == message_index;
                    std::string preview_path = !m.photos.empty() ? m.photos.front().thumbnail_path : m.media_path;
                    vita2d_texture* media = textures.get(preview_path);
                    bool has_media = !m.photos.empty() || m.media_file_id > 0 || m.animation;
                    int media_width = has_media ? 108 : 0;
                    int max_text_width = 680 - media_width;
                    auto lines = wrap_text(font, clip(m.text, 2048), max_text_width);
                    if (lines.size() > 2) {
                        lines.resize(2);
                        lines.back() = fit_text(font, lines.back() + "...", max_text_width, 0.68f);
                    }
                    int text_width = m.photos.size() > 1 ? 230 : 130;
                    for (const auto& line : lines) text_width = std::max(text_width, rich_text_width(font, 0.68f, line.c_str()));
                    int bubble_width = std::min(712, text_width + media_width + 32);
                    if (show_sender) bubble_width = std::max(bubble_width, rich_text_width(font, 0.55f, sender.c_str()) + 32);
                    int bubble_x = m.outgoing ? 928 - bubble_width : 32;
                    unsigned int bubble_color = m.outgoing ? (dark ? RGBA8(43,82,112,255) : RGBA8(211,242,207,255)) : white;
                    if (selected_message) rounded_rect(bubble_x-2, y-6, bubble_width+4, entry.height-2, accent);
                    rounded_rect(bubble_x, y-4, bubble_width, entry.height-6, bubble_color);
                    if (show_sender) {
                        static const unsigned int colors[] = {
                            static_cast<unsigned int>(RGBA8(25,115,156,255)),
                            static_cast<unsigned int>(RGBA8(135,72,162,255)),
                            static_cast<unsigned int>(RGBA8(36,128,93,255)),
                            static_cast<unsigned int>(RGBA8(167,86,37,255))
                        };
                        unsigned int sender_color = colors[static_cast<uint64_t>(m.sender_id)%4];
                        if (dark) sender_color = (sender_color & 0xFF000000u) | ((sender_color & 0x00FEFEFEu) >> 1) | 0x00808080u;
                        draw_text(font, bubble_x+16, y+13, sender, sender_color, 0.55f);
                        y += 20;
                    }
                    // Small tail points toward the sender's side.
                    vita2d_draw_rectangle(m.outgoing ? bubble_x+bubble_width-8 : bubble_x, y+54, 8, 14, bubble_color);
                    if (media) draw_texture_fit(media, (float)bubble_x+10, (float)y+4, 96, 56);
                    else if (has_media) draw_text(font, bubble_x+12, y+32, m.video_note ? "Round" : m.video ? "Video" : m.animation ? "GIF" : "...", muted, 0.68f);
                    int text_x = bubble_x + 16 + media_width;
                    for (size_t row = 0; row < lines.size(); ++row)
                        draw_text(font, text_x, y+20+(int)row*22, lines[row], ink, 0.68f);
                    if (m.photos.size() > 1) {
                        char album_count[32]; std::snprintf(album_count, sizeof(album_count), "%d photos", (int)m.photos.size());
                        draw_text(font, text_x, y + 61, album_count, muted, 0.52f);
                    }
                    std::string timestamp = message_time(m);
                    int time_right = bubble_x + bubble_width - (m.outgoing ? 42 : 12);
                    draw_text(font, time_right - rich_text_width(font, 0.52f, timestamp.c_str()), y+61, timestamp, muted, 0.52f);
                    if (m.outgoing) draw_delivery((float)(bubble_x+bubble_width-37), (float)y+50,
                                                  m.delivery, bubble_color, now);
                }
                vita2d_disable_clipping();
                if (msgs.empty()) draw_text(font, 38, 150, "No messages", muted, 0.75f);
                draw_text(font, 28, 528, "X  Open post", ink, 0.65f);
                draw_text(font, 275, 528, "Square: текст / держать: стикеры", ink, 0.5f);
                draw_text(font, 700, 528, "O  Back", ink, 0.65f);
            }
        } else if (tg.canBrowse()) {
            const auto& chats = tg.chats();
            if (folder_tabs) {
                int active = 0;
                for (int i = 0; i < (int)tg.folders().size(); ++i) if (tg.folders()[i].id == tg.selectedFolder()) active = i;
                if (shown_folder_id != tg.selectedFolder() || shown_folder_index != active) {
                    if (active * 180 < folder_scroll) folder_scroll = active * 180;
                    if ((active + 1) * 180 > folder_scroll + 920) folder_scroll = (active + 1) * 180 - 920;
                    shown_folder_id = tg.selectedFolder(); shown_folder_index = active;
                }
                folder_scroll = std::max(0, std::min(folder_scroll, folder_max_scroll));
                vita2d_enable_clipping();
                vita2d_set_clip_rectangle(20, 38, 940, 82);
                for (int i = 0; i < (int)tg.folders().size(); ++i) {
                    int x = 20 + i * 180 - folder_scroll;
                    if (x + 176 <= 20 || x >= 940) continue;
                    vita2d_draw_rectangle(x, 40, 176, 39, i == active ? selected : white);
                    draw_text(font, x+10, 65, fit_text(font, tg.folders()[i].title, 156, 0.65f), i == active ? accent : ink, 0.65f);
                    if (i == active) vita2d_draw_rectangle(x, 77, 176, 3, accent);
                }
                vita2d_disable_clipping();
                if (folder_scroll > 0) draw_text(font, 2, 66, "<", accent, 0.6f);
                if (folder_scroll < folder_max_scroll) draw_text(font, 944, 66, ">", accent, 0.6f);
            } else { shown_folder_id = -1; shown_folder_index = -1; }
            int y = chat_top + 24;
            int max_items = chat_rows;
            int base = 0;
            if (chat_index >= max_items) base = chat_index - max_items + 1;
            if (!decode_cooldown) for (int i = base; i < (int)chats.size() && i < base + max_items; ++i) tg.ensureChatMedia((size_t)i);
            for (int i = base; i < (int)chats.size() && i < base + max_items; ++i) {
                vita2d_draw_rectangle(20, y - 24, 920, chat_row_height, i == chat_index ? selected : white);
                vita2d_draw_rectangle(88, y - 25 + chat_row_height, 852, 1, separator);
                vita2d_texture* avatar = textures.get(chats[i].avatar_path);
                if (avatar) draw_texture_fit(avatar, 34, (float)y - 21, 42, 42);
                else vita2d_draw_rectangle(34, y - 21, 42, 42, RGBA8(126,176,211,255));
                draw_text(font, 88, y + 3, clip(chats[i].title, 70), ink, 0.72f);
                y += chat_row_height;
            }
            if (chats.empty()) draw_text(font, 38, chat_top+51, tg.chatsLoading() ? "Loading first chats..." :
                (tg.chatsEnd() ? "No chats in this list" : "Unable to load chats. Triangle = retry"), muted, 0.75f);
            vita2d_draw_rectangle(0, 481, 960, 18, selected);
            char progress[140];
            std::snprintf(progress, sizeof(progress), "%s  |  Loaded %u chats%s",
                tg.chatsLoading() ? "Loading" : "Ready", (unsigned int)chats.size(),
                tg.chatsEnd() ? "  |  All chats loaded" : "  |  Tap here for more");
            draw_text(font, 28, 494, !update_notice.empty() ? fit_text(font, update_notice, 900, 0.48f) :
                tg.chatsError().empty() ? std::string(progress) : "Retry: " + tg.chatsError(), ink, 0.48f);
            if (tg.chatsLoading()) {
                float position = (float)((now / 2000) % 840);
                vita2d_draw_rectangle(position, 497, 120, 2, accent);
            }
            draw_text(font, 28, 528, "D-pad  Select", ink, 0.65f);
            draw_text(font, 300, 528, "X Open / Start Settings", ink, 0.6f);
            draw_text(font, 680, 528, "Triangle  Refresh", ink, 0.65f);
        } else {
            bool starting = tg.inputKind() == InputKind::None;
            draw_text(font, 30, 145, starting ? "Connecting to Telegram..." : "Telegram authorization", ink, 0.9f);
            draw_text(font, 30, 180, tg.closed() ? "Перезапустите VitaGram для входа в аккаунт" : clip(tg.status(), 100), muted, 0.7f);
            if (starting && net_ok && tg.lastError().empty()) {
                int phase = (int)((now / 100000) % 8);
                for (int dot = 0; dot < 8; ++dot)
                    vita2d_draw_rectangle(32 + dot * 24, 205, 14, 14, dot == phase ? accent : separator);
                draw_text(font, 30, 490, "Waiting for Telegram | Loaded 0 chats", muted, 0.65f);
            }
        }

        } // !settings_open

        if (!settings_open && !stickers_open && !tg.topicsOpen() && !photo_open && tg.currentChatId()) {
            vita2d_draw_rectangle(0, 481, 960, 18, selected);
            int progress = tg.historyPercent();
            vita2d_draw_rectangle(0, 497, 960.0f * progress / 100, 2, accent);
            char loading[96];
            std::snprintf(loading, sizeof(loading), "History block: %d%%%s", progress, tg.historyLoading() ? " - loading..." : " - ready");
            draw_text(font, 28, 494, !browser_error.empty() ? browser_error :
                (tg.historyError().empty() ? std::string(loading) : "History: " + tg.historyError()), ink, 0.48f);
        }
        if (!settings_open && (pad.buttons & SCE_CTRL_SELECT) && !kb.active()) {
            vita2d_draw_rectangle(0, 78, 960, 52, RGBA8(20,35,50,240));
            draw_text(font, 20, 99, cpu_scheduler::status(), header_text, 0.6f);
            draw_text(font, 20, 123, tg.mediaCacheStatus(), header_text, 0.6f);
        }
        if (!settings_open && !tg.ready() && tg.canBrowse() && !kb.active()) {
            vita2d_draw_rectangle(0, 78, 960, 26, RGBA8(20,35,50,240));
            draw_text(font, 20, 98, "Cached data - waiting for Telegram", header_text, 0.55f);
        }
        if (post_open && !photo_open && !reactions_open && !kb.active())
            draw_text(font, 750, 472, "Square: реакции", muted, 0.45f);
        if (reactions_open) {
            vita2d_draw_rectangle(0, 36, 960, 508, white);
            draw_text(font, 32, 80, "Реакции", ink, 0.85f);
            draw_text(font, 32, 112, fit_text(font, tg.reactionStatus(), 880, 0.6f), muted, 0.6f);
            const auto& choices = tg.reactionChoices();
            int begin = reaction_index/32*32;
            for (int i=begin; i<(int)choices.size() && i<begin+32; ++i) {
                int x=32+(i-begin)%8*112, y=130+(i-begin)/8*72;
                rounded_rect(x, y, 104, 64, i == reaction_index ? selected : separator);
                draw_text(font, x+32, y+43, choices[i], ink, 1.0f);
            }
            draw_text(font, 28, 528, "D-pad: выбор   X: поставить / убрать   O: назад", ink, 0.6f);
        }
        if (tg.topicsOpen()) {
            vita2d_draw_rectangle(0, 36, 960, 508, white);
            draw_text(font, 30, 61, "Темы группы", ink, 0.7f);
            const auto& topics = tg.topics();
            int first = std::max(0, topic_index-7);
            for (int i = first; i < std::min(first+8, (int)topics.size()); ++i) {
                int y = 70+(i-first)*50;
                vita2d_draw_rectangle(20, y, 920, 48, i == topic_index ? selected : white);
                std::string title = topics[i].title + (topics[i].closed ? " [закрыта]" : "");
                draw_text(font, 34, y+32, fit_text(font, title, 740, 0.75f), ink, 0.75f);
                if (topics[i].unread) draw_text(font, 830, y+32, std::to_string(topics[i].unread), accent, 0.7f);
            }
            draw_text(font, 30, 492, fit_text(font, tg.topicsStatus(), 900, 0.55f), muted, 0.55f);
            draw_text(font, 30, 528, "X: открыть   Triangle: ещё темы   O: назад", ink, 0.6f);
        }
        if (stickers_open) {
            vita2d_draw_rectangle(0, 36, 960, 508, white);
            const auto& packs = tg.stickerPacks();
            std::string title = sticker_pack < (int)packs.size() ? packs[sticker_pack].title : "Стикеры";
            draw_text(font, 30, 76, fit_text(font, "L  " + title + "  R", 900, 0.8f), ink, 0.8f);
            draw_text(font, 30, 112, fit_text(font, tg.stickerStatus(), 900, 0.6f), muted, 0.6f);
            const auto& items = tg.stickers();
            int first = (sticker_index/18)*18;
            for (int i = first; i < std::min(first+18, (int)items.size()); ++i) {
                int x = 30+(i-first)%6*150, y = 130+(i-first)/6*110;
                vita2d_draw_rectangle(x, y, 144, 104, i == sticker_index ? selected : white);
                if (!decode_cooldown) tg.ensureSticker(i);
                auto texture = textures.get(items[i].path);
                if (texture) {
                    float w = vita2d_texture_get_width(texture), h = vita2d_texture_get_height(texture);
                    float scale = std::min(130.0f/w, 90.0f/h);
                    draw_texture_fit(texture, x+(144-w*scale)/2, y+(104-h*scale)/2, 130, 90);
                } else draw_text(font, x+45, y+60, items[i].emoji.empty() ? "..." : items[i].emoji, ink, 0.85f);
            }
            draw_text(font, 30, 490, "Страница " + std::to_string(first/18+1) + " / " + std::to_string(std::max(1, ((int)items.size()+17)/18)), muted, 0.6f);
            draw_text(font, 30, 528, "D-pad: выбор   L/R: набор   X: отправить   O: назад", ink, 0.6f);
        }
        if (kb.active()) kb.draw(font);

        vita2d_end_drawing();
        if (kb.active()) vita2d_common_dialog_update();
        vita2d_swap_buffers();
        textures.loadOne();
        if (!decode_cooldown) emoji_load_one();
    }

    vita2d_wait_rendering_done();
    photo_view.updateMotion(false, 0, false);
    animation.shutdown();
    voice.shutdown();
    textures.clear();
    rich_text_shutdown();
    vita2d_free_pgf(font);
    vita2d_fini();
    tg.stopWorker();
    net.shutdown();
    sceAppUtilShutdown();
    sceKernelExitProcess(0);
    return 0;
}
