#include "performance_log.h"
#include "i18n.h"
#include "exit_gesture.h"
#include "telegram_client.h"
#include "history_selection.h"
#include "chat_list_scroll.h"
#include "keyboard.h"
#include "vita_net.h"
#include "media_render.h"
#include "text_layout_cache.h"
#include "image_decoder.h"
#include <cstring>
#include "animation_player.h"
#include "voice_player.h"
#include "audio_recorder.h"
#include "transient_status.h"
#include "video_note_recorder.h"
#include "video_quality.h"
#include "config.h"
#include <memory>
#include "cpu_scheduler.h"
#include "settings.h"
#include "file_download.h"
#include "download_folder_picker.h"
#include "upload_picker.h"
#include "proxy_link.h"
#include "secure_store.h"
#include "vault.h"
#include "account_cleanup.h"
#include "qr_encode.h"
#include "vault_ui.h"
#include "qr_scanner.h"
#include "photo_view.h"
#include "updater.h"
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
#include <cmath>
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
        decoder_.request(requests_, paused_);
        std::string path;
        DecodedImage image;
        if (!decoder_.take(path, image)) return;
        performance::Scope measurement(performance::Metric::ImageUpload);
        // CPU decoding never touches vita2d; upload only between completed frames.
        vita2d_wait_rendering_done();
        vita2d_texture* texture = nullptr;
        if (image.width && image.height && image.rgba.size() == size_t(image.width)*image.height*4) {
            texture = vita2d_create_empty_texture(image.width, image.height);
            if (texture) {
                auto* pixels = static_cast<unsigned char*>(vita2d_texture_get_datap(texture));
                size_t stride = vita2d_texture_get_stride(texture);
                for (unsigned y=0; y<image.height; ++y)
                    std::memcpy(pixels+y*stride, image.rgba.data()+size_t(y)*image.width*4, size_t(image.width)*4);
                vita2d_texture_set_filters(texture, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
            }
        }
        size_t bytes = texture ? (size_t)vita2d_texture_get_stride(texture) * vita2d_texture_get_height(texture) : 0;
        // vita2d rounds every CDRAM allocation up to 256 KiB: a 128x128 sticker preview
        // really costs 256 KiB, so count that, not the pixel bytes (logs showed free
        // CDRAM falling from 90 MiB to 31 MiB while the cache believed it held 16 MiB).
        bytes = (bytes + 0x3ffffu) & ~size_t(0x3ffffu);
        if (bytes > 16 * 1024 * 1024) { vita2d_free_texture(texture); texture = nullptr; bytes = 0; }
        // 32 MiB of real CDRAM, leaving the rest to video surfaces and fonts.
        while (!items_.empty() && (used_bytes_ + bytes > 32 * 1024 * 1024 || items_.size() >= 256)) {
            auto oldest = std::min_element(items_.begin(), items_.end(), [](const CachedTexture& a, const CachedTexture& b) {
                return a.used < b.used;
            });
            if (oldest->texture) vita2d_free_texture(oldest->texture);
            used_bytes_ -= oldest->bytes;
            items_.erase(oldest);
        }
        items_.push_back({path, texture, clock_, bytes});
        used_bytes_ += bytes;
        performance::cdram_images = (int64_t)used_bytes_;
    }
    void stop() { decoder_.stop(); }
    void clear() {
        decoder_.clear();
        requests_.clear();
        for (auto& item : items_) if (item.texture) vita2d_free_texture(item.texture);
        items_.clear();
        used_bytes_ = 0;
        performance::cdram_images = 0;
    }
private:
    ImageDecoder decoder_;
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
    performance::SlowScope slow(performance::SlowPhase::Ui,"text_fit");
    return fit_utf8_text(text,width,[&](const std::string& value){return rich_text_width(font,scale,value.c_str());});
}

static std::vector<std::string> wrap_text(vita2d_pgf* font, const std::string& text, int width) {
    performance::SlowScope slow(performance::SlowPhase::Ui,"text_wrap");
    struct Entry { std::string text; int width; std::vector<std::string> lines; };
    static std::vector<Entry> cache;
    static int font_probe = 0;
    static unsigned font_revision = 0;
    int probe = rich_text_width(font, 1.0f, "Font size");
    if (probe != font_probe || rich_text_revision() != font_revision) {
        cache.clear(); font_probe = probe; font_revision = rich_text_revision();
    }
    for (const auto& entry : cache) if (entry.width == width && entry.text == text) return entry.lines;
    std::vector<std::string> lines;
    // Measuring every candidate line from scratch is quadratic in the paragraph
    // length (100+ ms for a long post on the Vita). Add per-word widths and
    // re-measure the whole line only when the sum lands near the limit, so
    // kerning and fallback fonts still decide the borderline cases.
    constexpr int margin = 12;
    const int space_width = rich_text_width(font, 0.68f, " ");
    auto fits = [&](const std::string& line, int line_width, const std::string& piece, int piece_width, int gap, std::string& joined) {
        const int estimate = line_width + gap + piece_width;
        if (estimate <= width - margin) return true;
        if (estimate > width + margin) return false;
        joined = line + (gap ? " " : "") + piece;
        return rich_text_width(font, 0.68f, joined.c_str()) <= width;
    };
    std::istringstream paragraphs(text);
    std::string paragraph;
    while (std::getline(paragraphs, paragraph)) {
    std::istringstream input(paragraph);
    std::string word, line, joined;
    int line_width = 0;
    while (input >> word) {
        const int word_width = rich_text_width(font, 0.68f, word.c_str());
        if (!line.empty() && !fits(line, line_width, word, word_width, space_width, joined)) {
            lines.push_back(line);
            line.clear(); line_width = 0;
        }
        // Split long URLs/words only on UTF-8 character boundaries.
        if (word_width > width) {
            for (size_t i = 0; i < word.size();) {
                size_t end = i + 1;
                while (end < word.size() && (static_cast<unsigned char>(word[end]) & 0xc0) == 0x80) ++end;
                std::string glyph = word.substr(i, end - i);
                const int glyph_width = rich_text_width(font, 0.68f, glyph.c_str());
                if (!line.empty() && !fits(line, line_width, glyph, glyph_width, 0, joined)) {
                    lines.push_back(line); line.clear(); line_width = 0;
                }
                line += glyph; line_width += glyph_width; i = end;
            }
            continue;
        }
        if (!line.empty()) { line += ' '; line_width += space_width; }
        line += word; line_width += word_width;
    }
    lines.push_back(line);
    }
    if (lines.empty()) lines.push_back("");
    if (cache.size() >= 48) cache.erase(cache.begin());
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

static unsigned int scale_alpha(unsigned int color, float factor) {
    const float a = std::max(0.f, std::min(1.f, factor)) * (float)(color >> 24);
    return (color & 0x00FFFFFFu) | ((unsigned int)a << 24);
}
static unsigned int mix_color(unsigned int a, unsigned int b, float t) {
    auto channel = [&](int shift) { return (unsigned int)((float)((a >> shift) & 255) * (1 - t) + (float)((b >> shift) & 255) * t); };
    return channel(0) | (channel(8) << 8) | (channel(16) << 16) | (channel(24) << 24);
}

// Translucent rounded panel with a vertical gradient, one non-overlapping stripe per row
// (overlapping fills would double the alpha).
static void glass_panel(float x, float y, float w, float h, unsigned int top, unsigned int bottom, float r = 9) {
    for (int i = 0; i < (int)h; ++i) {
        const float edge = (float)std::min(i + 0.5f, h - i - 0.5f);
        const float inset = edge >= r ? 0.f : r - std::sqrt(std::max(0.f, r * r - (r - edge) * (r - edge)));
        vita2d_draw_rectangle(x + inset, y + i, w - 2 * inset, 1, mix_color(top, bottom, h > 1 ? i / (h - 1) : 0));
    }
}

static void draw_paperclip(float cx, float cy, float scale, unsigned int color) {
    // Three nested U-bends, drawn as a polyline in a 11x21 box and rotated by 45 degrees.
    std::vector<std::pair<float, float>> pts;
    auto line = [&](float x, float y) { pts.emplace_back(x, y); };
    auto arc = [&](float ax, float ay, float r, float from, float to) {
        for (int i = 0; i <= 10; ++i) {
            const float t = (from + (to - from) * i / 10) * 3.14159265f / 180.0f;
            line(ax + r * std::cos(t), ay + r * std::sin(t));
        }
    };
    line(3, 15); line(3, 5);
    arc(5, 5, 2, 180, 360);
    line(7, 17);
    arc(4, 17, 3, 0, 180);
    line(1, 4);
    arc(5.5f, 4, 4.5f, 180, 360);
    line(10, 19);
    const float c = 0.70710678f;
    for (size_t i = 1; i < pts.size(); ++i) {
        float p[4] = {pts[i-1].first - 5.5f, pts[i-1].second - 10.5f, pts[i].first - 5.5f, pts[i].second - 10.5f};
        float q[4];
        for (int k = 0; k < 4; k += 2) {
            q[k] = cx + (p[k] * c - p[k+1] * c) * scale;
            q[k+1] = cy + (p[k] * c + p[k+1] * c) * scale;
        }
        vita2d_draw_line(q[0], q[1], q[2], q[3], color);
        vita2d_draw_line(q[0] + 1, q[1], q[2] + 1, q[3], color);
        vita2d_draw_line(q[0], q[1] + 1, q[2], q[3] + 1, color);
    }
}

// Quarter circle in a bottom corner: gray radial gradient, translucent body and a glossy ring rim
// with two glints. `glow` (0..1) brightens the ring while its menu is open.
static void draw_corner_ring(bool right, bool dark, float glow) {
    const float R = 66, deg = 3.14159265f / 180.0f, cx = right ? 960.f : 0.f;
    vita2d_draw_fill_circle(cx, 544, R + 5, RGBA8(255, 255, 255, dark ? 6 : 10));
    vita2d_draw_fill_circle(cx, 544, R + 3, RGBA8(255, 255, 255, dark ? 9 : 14));
    const int steps = 18;
    vita2d_draw_fill_circle(cx, 544, R, RGBA8(30, 34, 40, 150));
    for (int i = 1; i <= steps; ++i) {
        const float t = (float)i / steps;
        const unsigned int c = mix_color(RGBA8(62, 68, 78, 255), RGBA8(150, 158, 170, 255), t);
        vita2d_draw_fill_circle(cx, 544, R * (1.0f - 0.94f * t), (c & 0x00FFFFFFu) | (14u << 24));
    }
    // Visible arc: -90..0 degrees on the left button, -180..-90 on the right one (mirror image).
    const int from = right ? -180 : -90;
    for (int layer = 0; layer < 3; ++layer) {
        const float radius = R - 0.5f - layer;
        for (int a = from; a < from + 90; a += 2) {
            const float m = (float)(right ? -180 - a : a);   // mirrored angle for the glint profile
            const float g1 = std::exp(-((m + 62.f) * (m + 62.f)) / (2 * 14.f * 14.f));
            const float g2 = std::exp(-((m + 14.f) * (m + 14.f)) / (2 * 8.f * 8.f));
            float alpha = 70.f + 160.f * g1 + 110.f * g2 + 60.f * glow;
            alpha *= layer == 0 ? 1.0f : layer == 1 ? 0.6f : 0.3f;
            vita2d_draw_line(cx + radius * std::cos(a * deg), 544 + radius * std::sin(a * deg),
                             cx + radius * std::cos((a + 2) * deg), 544 + radius * std::sin((a + 2) * deg),
                             RGBA8(255, 255, 255, (unsigned int)std::min(255.f, alpha)));
        }
    }
    for (int a = from + 2; a < from + 86; a += 3)
        vita2d_draw_line(cx + (R - 7) * std::cos(a * deg), 544 + (R - 7) * std::sin(a * deg),
                         cx + (R - 7) * std::cos((a + 3) * deg), 544 + (R - 7) * std::sin((a + 3) * deg), RGBA8(255, 255, 255, 22));
}

static void draw_attach_button(bool dark, float glow = 0) {
    draw_corner_ring(false, dark, glow);
    draw_paperclip(26, 519, 1.55f, RGBA8(0, 0, 0, 90));
    draw_paperclip(25, 518, 1.55f, RGBA8(244, 247, 251, 255));
}

// Three dots, as on the original system menu button.
static void draw_menu_button(bool dark, float glow = 0) {
    draw_corner_ring(true, dark, glow);
    for (int i = 0; i < 3; ++i) {
        vita2d_draw_fill_circle(924.f + i * 11 + 1, 521, 3.2f, RGBA8(0, 0, 0, 90));
        vita2d_draw_fill_circle(924.f + i * 11, 520, 3.2f, RGBA8(244, 247, 251, 255));
    }
}

// One entry of the right-hand context menu: label key (text carries the button icon), the physical
// button it stands for, or a special action.
struct ChatMenuItem { const char* label; unsigned int button; int special; };
static constexpr int kMenuRowHeight = 38, kMenuX = 600, kMenuWidth = 320, kMenuBottom = 486;

struct PostReaction { std::string text; int width; bool chosen; int64_t custom_emoji_id; };
struct PostLine {
    std::string text, url;
    std::vector<PostReaction> reactions;
    PostLine(std::string value, std::string link) : text(std::move(value)), url(std::move(link)) {}
};

static void draw_post_line(vita2d_pgf* font, int x, int baseline, const PostLine& line,
                           unsigned int ink, unsigned int accent, TelegramClient& tg,
                           TextureCache& textures, bool pause_media) {
    if (line.reactions.empty()) {
        draw_text(font, x, baseline, line.text, line.url.empty() ? ink : accent, 0.68f);
        return;
    }
    if (!pause_media) tg.ensureMessageReactionMedia();
    for (const auto& reaction : line.reactions) {
        if (reaction.chosen) rounded_rect(x, baseline-18, reaction.width, 22, accent);
        int text_x=x+7;
        if (reaction.custom_emoji_id) {
            auto icon=textures.get(tg.customReactionIcon(reaction.custom_emoji_id));
            if (icon) draw_texture_fit(icon,x+7,baseline-18,20,20);
            else vita2d_draw_fill_circle(x+17,baseline-8,4,
                reaction.chosen ? RGBA8(255,255,255,255) : ink);
            text_x+=24;
        }
        draw_text(font, text_x, baseline, reaction.text, reaction.chosen ? RGBA8(255,255,255,255) : ink, 0.68f);
        x += reaction.width + 6;
    }
}

static std::string message_time(const MessageItem& message, bool full = false) {
    if (message.date <= 0) return "--:--";
    SceDateTime date{};
    SceRtcTick utc{}, local{};
    if (sceRtcSetTime_t(&date, static_cast<time_t>(message.date)) < 0 || sceRtcGetTick(&date, &utc) < 0)
        return "--:--";
    bool local_ok = sceRtcConvertUtcToLocalTime(&utc, &local) >= 0 && sceRtcSetTick(&date, &local) >= 0;
    char text[96];
    const std::string edited = message.edit_date ? " " + tr("message.edited") : "";
    if (full) std::snprintf(text, sizeof(text), "%02u.%02u.%04u  %02u:%02u%s%s",
        (unsigned)date.day, (unsigned)date.month, (unsigned)date.year, (unsigned)date.hour, (unsigned)date.minute,
        local_ok ? "" : " UTC", edited.c_str());
    else std::snprintf(text, sizeof(text), "%02u:%02u%s", (unsigned)date.hour, (unsigned)date.minute, local_ok ? "" : " UTC");
    return text;
}

static std::string message_day(const MessageItem& message) {
    if (message.date <= 0) return tr("date.unknown");
    SceDateTime date{}; SceRtcTick utc{}, local{};
    if (sceRtcSetTime_t(&date, static_cast<time_t>(message.date)) < 0 || sceRtcGetTick(&date, &utc) < 0) return tr("date.unknown");
    bool local_ok = sceRtcConvertUtcToLocalTime(&utc, &local) >= 0 && sceRtcSetTick(&date, &local) >= 0;
    if (date.month < 1 || date.month > 12) return tr("date.unknown");
    return tr("date.format", {std::to_string(date.day), tr(("month." + std::to_string(date.month)).c_str()),
        std::to_string(date.year)}) + (local_ok ? "" : " UTC");
}

struct HistoryRow { int index, top, height, day_top; std::string day; };
// Shared by drawing and touch hit testing; date pills are not messages.
static std::vector<HistoryRow> history_layout(const std::vector<MessageItem>& messages, int selected, bool group) {
    if (messages.empty()) return {};
    selected = std::max(0, std::min(selected, (int)messages.size()-1));
    int begin = std::max(0, selected-5);
    for (;;) {
        std::vector<HistoryRow> rows;
        int y = 58;
        std::string previous;
        for (int i = begin; i < (int)messages.size(); ++i) {
            std::string day = message_day(messages[i]);
            bool separator = day != previous;
            int height = (group && !messages[i].outgoing ? 98 : 78) +
                (messages[i].media_label.empty() ? 0 : 22);
            if (messages[i].service_message) height = 36;
            int day_top = separator ? y : -1;
            int top = y + (separator ? 26 : 0);
            if (top >= 544) break;
            rows.push_back({i, top, height, day_top, day});
            y = top + height; previous = day;
        }
        bool selected_fits = false;
        for (const auto& row : rows) if (row.index == selected && row.top + row.height <= 544) selected_fits = true;
        if (begin >= selected || selected_fits) return rows;
        ++begin;
    }
}

static std::string delivery_label(DeliveryState state) {
    switch (state) {
        case DeliveryState::Pending: return tr("delivery.sending");
        case DeliveryState::Read: return tr("delivery.read");
        case DeliveryState::Failed: return tr("delivery.failed");
        default: return tr("delivery.sent");
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
    if (message.transcription_complete && message.transcription.empty())
        result.push_back({tr("post.transcription_empty"), ""});
    else if (((message.voice && !message.audio) || message.video_note) && message.transcription.empty())
        result.push_back({message.transcription_pending ? tr("post.transcription_wait") : tr("post.transcribe"),
            message.transcription_pending ? "" : "vitagram:transcribe/" + std::to_string(message.id)});
    if (message.document) result.push_back({tr("hint.download"), ""});
    PostLine reaction_line("", "");
    int reaction_width = 0;
    for (const auto& reaction : message.reactions) {
        std::string label = (reaction.custom_emoji_id ? "" : reaction.emoji + " ") + std::to_string(reaction.count);
        int width = rich_text_width(font, 0.68f, label.c_str()) + 14 + (reaction.custom_emoji_id ? 24 : 0);
        if (!reaction_line.reactions.empty() && reaction_width + width > 840) {
            result.push_back(std::move(reaction_line));
            reaction_line = PostLine("", ""); reaction_width = 0;
        }
        reaction_line.reactions.push_back({label, width, reaction.chosen, reaction.custom_emoji_id});
        reaction_width += width + 6;
    }
    if (!reaction_line.reactions.empty()) result.push_back(std::move(reaction_line));
    if (message.delivery == DeliveryState::Failed && !message.send_error.empty())
        for (const auto& line : wrap_text(font, message.send_error, 840)) result.push_back({line, ""});
    if (!message.transcription.empty()) {
        result.push_back({message.transcription_pending ? tr("post.transcription_pending") : tr("post.transcription"), ""});
        for (const auto& line : wrap_text(font, message.transcription, 840)) result.push_back({line, ""});
    }
    for (const auto& line : wrap_text(font, message.text, 840)) {
        std::string target;
        int matches = 0;
        for (const auto& url : message.links) if (line.find(url) != std::string::npos) { target = url; ++matches; }
        result.push_back({line, matches == 1 ? target : ""});
    }
    // Separate rows keep multiple and hidden-text links individually tappable.
    for (const auto& url : message.links) result.push_back({fit_text(font, tr("hint.open") + url, 840, 0.68f), url});
    return result;
}

static bool scrollable_post(const MessageItem& message) {
    return !message.voice;
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
static constexpr int post_media_top = 80;
static constexpr int post_media_height = 340;
static int post_text_top(const MessageItem& message) {
    if (!message.photos.empty()) return 294;
    return message.animation || message.sticker ? 466 : 98;
}
static void draw_post_date(vita2d_pgf* font, const MessageItem& message, int scroll,
                           unsigned int background, unsigned int color) {
    std::string label = message_day(message) + "  " + message_time(message);
    int width = rich_text_width(font, 0.55f, label.c_str()) + 28;
    rounded_rect(480-width/2, 57-scroll, width, 22, background);
    draw_text(font, 494-width/2, 73-scroll, label, color, 0.5f);
}
static int post_scroll_max(const MessageItem& message, size_t lines) {
    return std::max(0, post_text_top(message) + (int)lines*24 - 544);
}

static std::string open_web_link(const std::string& url) {
    std::string lower = url.substr(0, 8);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if ((lower.compare(0, 7, "http://") && lower != "https://") || url.find_first_of("\r\n\t ") != std::string::npos)
        return tr("link.invalid");
    int rc = sceAppMgrLaunchAppByUri(0x20000, ("webmodal: " + url).c_str());
    if (rc >= 0) return "";
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08X", (unsigned)rc);
    return tr("link.browser_error", {code});
}

// Korean and Chinese need their own glyphs; other languages use the standard font chain.
static void apply_language_font() {
    const std::string language = i18n::current();
    const char* path = language == "ko" ? "app0:assets/NotoSansKR-Regular.otf" :
        language == "zh-Hans" ? "app0:assets/NotoSansSC-Regular.otf" :
        language == "zh-Hant" ? "app0:assets/NotoSansTC-Regular.otf" : nullptr;
    rich_text_set_language_font(path);
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

// Settings list: language, 3 cache rows... 14 rows in total; the stride leaves room for the footer.
constexpr int kSettingsRows = 15, kSettingsStride = 25;

enum ProxyRow { PROXY_ROW_USE, PROXY_ROW_TYPE, PROXY_ROW_SERVER, PROXY_ROW_PORT, PROXY_ROW_SECRET,
                PROXY_ROW_USER, PROXY_ROW_PASS, PROXY_ROW_SAVE, PROXY_ROW_SCAN, PROXY_ROW_CHECK };

// Rows shown on the proxy screen: MTProxy has a secret, SOCKS5/HTTPS have optional credentials.
static std::vector<int> proxy_rows(int type) {
    std::vector<int> rows{PROXY_ROW_USE, PROXY_ROW_TYPE, PROXY_ROW_SERVER, PROXY_ROW_PORT};
    if (type == 0) rows.push_back(PROXY_ROW_SECRET);
    else { rows.push_back(PROXY_ROW_USER); rows.push_back(PROXY_ROW_PASS); }
    rows.insert(rows.end(), {PROXY_ROW_SAVE, PROXY_ROW_SCAN, PROXY_ROW_CHECK});
    return rows;
}

int main() {
    pthread_init();
    // A staged update is installed now, while almost no memory is in use (see Updater::applyPending).
    std::string update_failure;
    switch (Updater::applyPending("ux0:data/vitagram/update", update_failure)) {
        case Updater::Pending::Applied: sceAppMgrLoadExec("app0:eboot.bin", nullptr, nullptr); sceKernelExitProcess(0); break;
        case Updater::Pending::Failed: break; // reported in Settings below
        case Updater::Pending::None: break;
    }
    const int64_t startup_time = performance::now();
    performance::collector().start(startup_time);
    cpu_scheduler::init();
    cpu_scheduler::Thread ui_thread(cpu_scheduler::Role::Ui);
    SceAppUtilInitParam app_util_init{};
    SceAppUtilBootParam app_util_boot{};
    sceAppUtilInit(&app_util_init, &app_util_boot);
    SceCommonDialogConfigParam dialog_config{};
    sceCommonDialogConfigParamInit(&dialog_config);
    sceCommonDialogSetConfigParam(&dialog_config);

    // Settings come first: the saved language ("" = follow the console language) must be known
    // before the first text is drawn.
    AppSettings settings;
    vault::load();
    // With the vault on, settings.json is sealed: it is read after the key has been entered, and
    // the key screens use the console language until then.
    if (!vault::enabled()) settings.load();
    i18n::init("app0:lang", settings.language);
    apply_language_font();

    vita2d_init();
    vita2d_set_vblank_wait(1);
    vita2d_set_clear_color(RGBA8(238, 243, 248, 255));
    vita2d_pgf* font = vita2d_load_default_pgf();
    startup_screen(font, "1/5  Loading fonts and emoji", 1);
    performance::Scope font_measurement(performance::Metric::FontsInit);
    if (!rich_text_init()) std::fprintf(stderr, "Noto text/emoji font initialization incomplete\n");
    font_measurement.finish();
    // Encryption gate: first-run question, key entry, and moving old files into/out of the vault.
    // It runs before the network and TDLib start so no file is changed while TDLib may use it.
    {
        const vault_ui::Env vault_env{font, settings.theme == 1};
        vault_ui::run_gate(vault_env);
        if (vault::enabled()) {
            settings.load();
            i18n::select(settings.language);
            apply_language_font();
        }
        vault_ui::run_migration(vault_ui::Env{font, settings.theme == 1});
    }
    startup_screen(font, "2/5  Initializing network", 2);
    VitaNet net;
    performance::Scope network_measurement(performance::Metric::NetworkInit);
    bool net_ok = net.init();
    network_measurement.finish();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    SceCtrlData pad{};
    unsigned int old_buttons = 0;

    ensure_download_directory("ux0:downloads/vitagram");
    TelegramClient tg;
    tg.configureProxy(settings.proxy_enabled, settings.proxyConfig());
    tg.setRequestBatchSize(settings.request_batch_size);
    startup_screen(font, "3/5  Starting Telegram transport", 3);
    if (net_ok) tg.start(true);

    // The main loop can process authorization, navigation and live updates while
    // the storage worker prepares a fallback snapshot. Fresh Telegram data wins.
    startup_screen(font, "4/5  Starting cache restore", 4);
    tg.restoreCacheAsync();

    Keyboard kb;
    startup_screen(font, "5/5  Loading settings", 5);
    rich_text_set_scale(settings.fontScale());
    bool settings_open = false, confirm_logout = false, confirm_vault_off = false;
    int settings_row = 0;
    Updater updater(VITAGRAM_VERSION, "app0:certs/cacert.pem", "ux0:data/vitagram/update");
    if (!update_failure.empty()) updater.reportStartupFailure(update_failure);
    else if (FILE* f = std::fopen("ux0:data/vitagram/update/result.txt", "rb")) { // from a failed install at an earlier start
        char text[160]; size_t n = std::fread(text, 1, sizeof(text) - 1, f); std::fclose(f); text[n] = 0;
        if (n) updater.reportStartupFailure(text);
    }
    const int64_t update_check_due = sceKernelGetSystemTimeWide() + 60000000; // stay out of the way of startup
    bool update_auto_checked = false, update_auto_install = false, restart_requested = false;
    int64_t update_installed_at = 0;
    auto update_error_text = [&](const Updater::Snapshot& u) {
        switch (u.error) {
            case Updater::Error::Network: return tr("update.err.network");
            case Updater::Error::NoRelease: return tr("update.err.no_release");
            case Updater::Error::BadSignature: return tr("update.err.signature");
            case Updater::Error::Storage: return tr("update.err.storage");
            case Updater::Error::Package: return tr("update.err.package");
            case Updater::Error::Install: return tr("update.err.install");
            case Updater::Error::Cancelled: return tr("update.err.cancelled");
            default: return tr("update.err.network");
        }
    };
    auto update_label = [&](const Updater::Snapshot& u) {
        switch (u.state) {
            case Updater::State::Idle: return tr("update.idle", {updater.currentVersion()});
            case Updater::State::Checking: return tr("update.checking");
            case Updater::State::UpToDate: return tr("update.up_to_date", {updater.currentVersion()});
            case Updater::State::Available: return tr("update.available", {u.version});
            case Updater::State::Downloading: return tr("update.downloading", {std::to_string(u.percent)});
            case Updater::State::Verifying: return tr("update.verifying");
            case Updater::State::Extracting: return tr("update.extracting", {std::to_string(u.percent)});
            case Updater::State::Installing: return tr("update.installing");
            case Updater::State::Installed: return tr("update.installed", {u.version});
            case Updater::State::Failed: return update_error_text(u);
        }
        return std::string();
    };
    bool proxy_open = false;
    int proxy_input = -1;
    DownloadFolderPicker folder_picker;
    UploadPicker upload_picker;
    int64_t attach_menu_since = 0;
    bool chat_menu_open = false;
    int chat_menu_sel = 0;
    int64_t chat_menu_since = 0;
    bool folder_name_input = false;
    AppSettings proxy_draft;
    QrScanner qr_scanner;
    bool proxy_return_to_screen = false, proxy_imported = false;
    std::string pending_proxy_link;
    auto open_link = [&](const std::string& url) -> std::string {
        if (url.compare(0, 20, "vitagram:transcribe/") == 0) {
            tg.requestTranscription(std::strtoll(url.c_str() + 20, nullptr, 10));
            return "";
        }
        if (is_proxy_link(url)) { pending_proxy_link = url; return ""; }
        if (tg.openTelegramLink(url)) return "";
        return open_web_link(url);
    };
    std::string settings_error;
    auto language_label = [&]() {
        auto name_of = [](const std::string& code) {
            for (const auto& language : i18n::languages()) if (language.code == code) return language.name;
            return code;
        };
        return settings.language.empty() ? tr("settings.language_auto", {name_of(i18n::current())}) : name_of(settings.language);
    };
    int64_t next_cache_cleanup = sceKernelGetSystemTimeWide() + 300000000;
    TextureCache textures;
    AnimationPlayer animation;
    VoicePlayer voice;
    AudioRecorder recorder;
    int64_t audio_chat = 0;
    int32_t audio_topic = 0;
    VideoNoteRecorder circle_recorder;
    std::unique_ptr<AnimationPlayer> circle_playback;
    int64_t circle_chat = 0;
    int32_t circle_topic = 0;
    std::string circle_error;
    int chat_index = 0;
    ChatListScroll chat_scroll;
    bool chat_drag = false;
    int chat_touch_y = 0;
    int message_index = -1;
    int photo_index = 0;
    int post_text_scroll = 0;
    bool post_open = false;
    bool reactions_open = false;
    bool stickers_open = false;
    bool sticker_pack_menu = false;
    int sticker_index = 0, sticker_pack = 0;
    int topic_index = 0;
    int64_t square_since = 0;
    bool square_long = false;
    int reaction_index = 0;
    int poll_index = 0;
    int64_t poll_message = 0, poll_chat = 0;
    std::vector<int> poll_choices;
    std::string poll_signature;
    bool photo_open = false;
    bool video_fullscreen = false;
    bool unsupported_video_prompt = false;
    int64_t unsupported_video_chat = 0, unsupported_video_message = 0;
    bool portrait_preparation_requested = false;
    int64_t portrait_preparation_chat = 0, portrait_preparation_message = 0;
    PhotoView photo_view;
    bool running = true;
    ExitGesture exit_gesture;
    bool touch_down = false, touch_dragged = false, touch_blocked = false;
    int touch_x = 0, touch_y = 0, touch_start_x = 0, touch_start_y = 0, touch_step_y = 0;
    unsigned int decode_cooldown = 0;
    int64_t next_network_poll = 0;
    std::string browser_error;
    TransientStatus browser_notice, audio_notice;
    int64_t status_chat = 0, status_topic = 0;
    int folder_scroll = 0, folder_touch_x = 0, shown_folder_index = -1;
    int32_t shown_folder_id = -1;
    bool folder_drag = false;

    auto open_circle = [&]() {
        if (recorder.recording() || recorder.processing()) {
            browser_error = tr("rec.finish_first"); return;
        }
        if (!tg.ready() || !tg.currentChatId()) { browser_error = tr("rec.wait_connect"); return; }
        circle_chat = tg.currentChatId(); circle_topic = tg.currentTopicId(); circle_error.clear();
        circle_playback.reset();
        animation.setSource(""); voice.setSource("");
        circle_recorder.open(std::string(VITAGRAM_FILES_DIR) + "/video-note-" +
            std::to_string(sceKernelGetSystemTimeWide()) + ".mp4");
    };

    bool startup_ready_measured = false;
    performance::observe(performance::Metric::StartupUi, performance::now()-startup_time);
    // Actions of the current chat screen; each maps to the physical button that already does it.
    auto chat_menu_items = [&]() {
        std::vector<ChatMenuItem> v;
        const auto& m = tg.messages();
        const bool has_message = message_index >= 0 && message_index < (int)m.size();
        if (circle_recorder.active()) {
            using S = VideoNoteRecorder::State;
            const S state = circle_recorder.state();
            if (state == S::Preview) v = {{"menu.rec_record", SCE_CTRL_CROSS, 0}, {"menu.rec_camera", SCE_CTRL_TRIANGLE, 0}};
            else if (state == S::Recording) v = {{"menu.rec_stop", SCE_CTRL_CROSS, 0}, {"menu.rec_camera", SCE_CTRL_TRIANGLE, 0}};
            else if (state == S::Review) v = {{"menu.rec_send", SCE_CTRL_CROSS, 0}, {"menu.rec_preview", SCE_CTRL_SQUARE, 0}};
            else if (state == S::Error) v = {{"menu.rec_retry", SCE_CTRL_CROSS, 0}};
            v.push_back({"menu.rec_cancel", SCE_CTRL_CIRCLE, 0});
        } else if (!tg.currentChatId() && !tg.topicsOpen()) {
            v = {{"menu.open_chat", SCE_CTRL_CROSS, 0}, {"menu.settings", SCE_CTRL_START, 0}, {"menu.refresh", SCE_CTRL_TRIANGLE, 0}};
            if (tg.folders().size() > 1) { v.push_back({"menu.prev_folder", SCE_CTRL_LTRIGGER, 0}); v.push_back({"menu.next_folder", SCE_CTRL_RTRIGGER, 0}); }
        } else if (tg.topicsOpen()) {
            v = {{"menu.open_topic", SCE_CTRL_CROSS, 0}, {"menu.load_topics", SCE_CTRL_TRIANGLE, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
        } else if (stickers_open) {
            if (sticker_pack_menu) v = {{"menu.open_set", SCE_CTRL_CROSS, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
            else v = {{"menu.send_sticker", SCE_CTRL_CROSS, 0}, {"menu.sets", SCE_CTRL_CIRCLE, 0}, {"menu.close_picker", SCE_CTRL_SQUARE, 0}};
        } else if (reactions_open) {
            v = {{"menu.react_toggle", SCE_CTRL_CROSS, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
        } else if (video_fullscreen) {
            v = {{"menu.play_pause", SCE_CTRL_CROSS, 0}, {"menu.normal_view", SCE_CTRL_TRIANGLE, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
        } else if (photo_open) {
            v = {{"menu.prev_photo_dpad", SCE_CTRL_LEFT, 0}, {"menu.next_photo_dpad", SCE_CTRL_RIGHT, 0},
                 {"menu.reset_zoom", SCE_CTRL_TRIANGLE, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
        } else if (post_open && has_message && m[message_index].poll.id) {
            v = {{"menu.poll_choose", SCE_CTRL_CROSS, 0}, {"menu.poll_send", SCE_CTRL_TRIANGLE, 0},
                 {"menu.poll_retract", SCE_CTRL_SQUARE, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
        } else if (post_open && has_message) {
            const MessageItem& item = m[message_index];
            v = {{"menu.prev_post", SCE_CTRL_LEFT, 0}, {"menu.next_post", SCE_CTRL_RIGHT, 0}};
            if (item.photos.size() > 1) { v.push_back({"menu.prev_photo", SCE_CTRL_LTRIGGER, 0}); v.push_back({"menu.next_photo", SCE_CTRL_RTRIGGER, 0}); }
            if (!item.photos.empty()) v.push_back({"menu.open_photo", SCE_CTRL_CROSS, 0});
            if (item.voice || item.video) v.push_back({"menu.play_pause", SCE_CTRL_CROSS, 0});
            if (item.video && !item.video_note) v.push_back({"menu.fullscreen", SCE_CTRL_TRIANGLE, 0});
            if (item.document) v.push_back({"menu.download", SCE_CTRL_CROSS, 0});
            v.push_back({"menu.react", SCE_CTRL_SQUARE, 0});
            v.push_back({"menu.back", SCE_CTRL_CIRCLE, 0});
        } else {
            if (has_message) v.push_back({"menu.open_post", SCE_CTRL_CROSS, 0});
            v.push_back({"menu.compose", SCE_CTRL_SQUARE, 0});
            v.push_back({"menu.stickers", 0, 1});
            v.push_back({"menu.video_note", SCE_CTRL_LTRIGGER, 0});
            v.push_back({"menu.audio", SCE_CTRL_RTRIGGER, 0});
            v.push_back({"menu.attach", SCE_CTRL_SELECT, 0});
            v.push_back({"menu.refresh", SCE_CTRL_TRIANGLE, 0});
            v.push_back({"menu.back", SCE_CTRL_CIRCLE, 0});
        }
        return v;
    };
    // Corner buttons exist on every chat screen except modal tools (recorder, scanner, file picker).
    auto chat_ui_visible = [&]() {
        return (tg.currentChatId() != 0 || tg.topicsOpen() || tg.canBrowse()) && !settings_open &&
               !qr_scanner.active() && !kb.active() && !unsupported_video_prompt &&
               (upload_picker.stage == UploadPicker::Stage::Closed || upload_picker.stage == UploadPicker::Stage::Menu);
    };
    while (running) {
        performance::Scope frame_measurement(performance::Metric::Frame);
        performance::Scope ui_measurement(performance::Metric::UiWork);
        performance::SlowScope slow_ui(performance::SlowPhase::Ui,"frame_ui");
        performance::SlowScope pre_input(performance::SlowPhase::Ui,"pre_input");
        int64_t now = sceKernelGetSystemTimeWide();
        if (!update_auto_checked && now >= update_check_due) {
            update_auto_checked = true;
            if (settings.check_updates && net_ok) { update_auto_install = true; updater.check(); }
        }
        {
            // Startup check found a release: install it unattended, then restart into the new version.
            const Updater::State update_now = updater.snapshot().state;
            if (update_auto_install && update_now == Updater::State::Available) { update_auto_install = false; updater.install(); }
            else if (update_auto_install && update_now != Updater::State::Checking) update_auto_install = false;
            if (update_now == Updater::State::Installed) {
                if (!update_installed_at) update_installed_at = now;
                // Wait a moment so the result is visible, and never interrupt typing, recording or an open chat.
                if (!restart_requested && now - update_installed_at > 3000000 && !kb.active() && !recorder.recording() &&
                    !recorder.processing() && !circle_recorder.active() && !tg.currentChatId()) {
                    restart_requested = true;
                    tg.close();
                    running = false;
                }
            }
        }
        // Switching encryption off completes once files are plain again and TDLib's database has
        // been re-keyed away from the vault key; only then may the vault key be dropped.
        if (vault::disabling() && vault::unlocked() && !vault::migration_pending() && tg.databaseKeyCurrent() &&
            (tg.canBrowse() || tg.inputKind() != InputKind::None)) vault::finish_disable();
        if (now >= next_network_poll) { net.poll(); next_network_poll = now + 1000000; }
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~old_buttons;
        const bool button_action = pressed != 0;
        SceTouchData touch{};
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        const bool interaction = button_action || touch.reportNum || touch_down || (old_buttons & ~pad.buttons);
        const int64_t input_sampled = interaction ? performance::now() : 0;
        if (interaction) tg.prioritizeInteraction(now);
        unsigned int released = old_buttons & ~pad.buttons;
        old_buttons = pad.buttons;
        bool compose_context = !circle_recorder.active() && tg.currentChatId() && !tg.topicsOpen() && !post_open && !photo_open && !settings_open &&
            !reactions_open && !stickers_open && !upload_picker.active() && !kb.active() && !recorder.recording() && !recorder.processing() && tg.inputKind() == InputKind::None;
        if (compose_context) {
            if (pressed & SCE_CTRL_SQUARE) { square_since = now; square_long = false; }
            pressed &= ~SCE_CTRL_SQUARE;
            if ((pad.buttons & SCE_CTRL_SQUARE) && square_since && now-square_since >= 550000) {
                square_long = true; square_since = 0;
                stickers_open = true; sticker_pack_menu = true; sticker_index = 0; sticker_pack = 1; tg.openStickers();
            }
            if (released & SCE_CTRL_SQUARE) {
                if (square_since && !square_long) pressed |= SCE_CTRL_SQUARE;
                square_since = 0;
            }
        } else square_since = 0;

        int64_t selected_chat = chat_index >= 0 && chat_index < (int)tg.chats().size() ? tg.chats()[chat_index].id : 0;
        const auto history_selection = HistorySelection::capture(tg.messages(), message_index);
        int64_t previous_last = tg.messages().empty() ? 0 : tg.messages().back().id;
        bool was_at_bottom = message_index < 0 || message_index + 1 == (int)tg.messages().size();
        bool was_cleaning = tg.cacheMaintenance();
        bool was_ready = tg.ready();
        pre_input.finish();
        tg.pump(interaction);
        if (tg.takeLinkChatOpened()) {
            post_open = photo_open = video_fullscreen = false;
            message_index = -1; photo_index = 0; post_text_scroll = 0;
            animation.setSource(""); voice.setSource("");
        }
        performance::SlowScope post_pump(performance::SlowPhase::Ui,"post_pump");
        if (!startup_ready_measured && tg.ready()) {
            startup_ready_measured = true;
            performance::observe(performance::Metric::StartupReady, performance::now()-startup_time);
        }
        if (was_ready && !tg.ready()) {
            recorder.cancel(); circle_playback.reset(); circle_recorder.cancel();
            animation.setSource(""); voice.setSource("");
            post_open = photo_open = video_fullscreen = false;
        }
        if (!tg.currentChatId()) reactions_open = false;
        if (!tg.currentChatId() || !tg.ready()) { stickers_open = false; upload_picker.close(); }
        if ((was_cleaning && !tg.cacheMaintenance()) || (was_ready && !tg.ready())) {
            vita2d_wait_rendering_done(); textures.clear();
        }
        cpu_scheduler::tick();
        if (selected_chat) {
            for (size_t i = 0; i < tg.chats().size(); ++i)
                if (tg.chats()[i].id == selected_chat) { chat_index = (int)i; break; }
        }
        chat_index = std::max(0, std::min(chat_index, (int)tg.chats().size() - 1));
        if (tg.takeChatTopRequest() && !tg.currentChatId()) { chat_index = 0; chat_scroll.reset(); }
        const int restored_message = history_selection.find(tg.messages());
        if (restored_message >= 0) message_index = restored_message;
        if (was_at_bottom && !post_open && !photo_open && !tg.messages().empty() &&
            tg.messages().back().outgoing && tg.messages().back().id != previous_last)
            message_index = (int)tg.messages().size() - 1;
        if (const int64_t target = tg.takeMessageJump()) {
            for (size_t i = 0; i < tg.messages().size(); ++i) if (tg.messages()[i].id == target) {
                message_index = (int)i;
                post_open = true; photo_open = video_fullscreen = reactions_open = false;
                photo_index = 0; post_text_scroll = 0;
                animation.setSource(""); voice.setSource("");
                break;
            }
        }

        const bool folder_tabs = !tg.folders().empty();
        const int chat_rows = folder_tabs ? 9 : 10;
        const int chat_top = folder_tabs ? 108 : 66;   // below the 18 px status strip (and the tabs)
        const int chat_row_height = (544-chat_top)/chat_rows;
        chat_scroll.configure((int)tg.chats().size(), chat_row_height, 544-chat_top);
        const int folder_max_scroll = std::max(0, (int)tg.folders().size() * 180 - 920);
        folder_scroll = std::max(0, std::min(folder_scroll, folder_max_scroll));
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
                touch_blocked = kb.active() || (!circle_recorder.active() && !settings_open && tg.canBrowse() && tg.inputKind() != InputKind::None);
                folder_drag = !circle_recorder.active() && !stickers_open && !settings_open && folder_tabs && !tg.currentChatId() && touch_start_y >= 56 && touch_start_y < 100;
                folder_touch_x = touch_x;
                chat_drag = !circle_recorder.active() && !stickers_open && !settings_open &&
                    !reactions_open && !upload_picker.active() && !tg.currentChatId() && tg.canBrowse() &&
                    !touch_blocked && touch_start_y >= chat_top && touch_start_y < 544;
                chat_touch_y = touch_y;
            }
            if (std::abs(touch_y - touch_start_y) > 14 || std::abs(touch_x - touch_start_x) > 14) touch_dragged = true;
            if (folder_drag && !touch_blocked && touch_dragged) {
                folder_scroll = std::max(0, std::min(folder_max_scroll, folder_scroll + folder_touch_x - touch_x));
            }
            folder_touch_x = touch_x;
            if (chat_drag && touch_dragged &&
                std::abs(touch_y - touch_start_y) >= std::abs(touch_x - touch_start_x)) {
                chat_scroll.drag(touch_y - chat_touch_y);
                if (chat_scroll.end() > chat_scroll.first())
                    chat_index = std::clamp(chat_index, chat_scroll.first(), chat_scroll.end()-1);
                if (chat_scroll.end() >= (int)tg.chats().size() - 2 && touch_y < chat_touch_y)
                    tg.loadMoreChats();
            }
            // Include the initial movement once the tap threshold is crossed.
            if (chat_drag && touch_dragged) chat_touch_y = touch_y;
            if (!chat_drag && !folder_drag && !touch_blocked && touch_start_y >= 38 && touch_start_y < 544 && !photo_open && !video_fullscreen &&
                std::abs(touch_y - touch_start_y) >= std::abs(touch_x - touch_start_x) &&
                std::abs(touch_y - touch_step_y) >= 32) {
                pressed |= touch_y < touch_step_y ? SCE_CTRL_DOWN : SCE_CTRL_UP;
                touch_step_y = touch_y;
            }
        } else if (touch_down && !touch_blocked && !photo_gesture) {
            tap = !touch_dragged;
            if (!video_fullscreen && (photo_open || post_open) && std::abs(touch_x - touch_start_x) > 45 &&
                std::abs(touch_x - touch_start_x) > std::abs(touch_y - touch_start_y))
                pressed |= touch_x < touch_start_x ? SCE_CTRL_RIGHT : SCE_CTRL_LEFT;
        }
        touch_down = touching;
        if (touching || pressed) decode_cooldown = 8;
        else if (decode_cooldown) --decode_cooldown;
        textures.beginFrame(interaction || decode_cooldown != 0 || kb.active() || circle_recorder.active());

        post_pump.finish();
        performance::SlowScope input_actions(performance::SlowPhase::Ui,settings_open ? "input_settings" : photo_open ? "input_photo" : post_open ? "input_post" : "input_browser");
        // Dismiss previous results before handling input, so a new error from
        // this action can still appear, including one with the same text.
        if (button_action || pressed || tap || touching) {
            browser_error.clear(); browser_notice.clear();
            tg.dismissStatusNotices();
            if (!recorder.recording() && !recorder.processing()) {
                recorder.setStatus(""); audio_notice.clear();
            }
        }

        {
            const bool ui = chat_ui_visible();
            auto open_menu = [&]() {
                if (upload_picker.stage == UploadPicker::Stage::Menu) upload_picker.close();
                chat_menu_open = true; chat_menu_sel = 0; chat_menu_since = now;
            };
            auto run_item = [&](const ChatMenuItem& item) {
                chat_menu_open = false;
                if (item.special == 1) { stickers_open = true; sticker_pack_menu = true; sticker_index = 0; sticker_pack = 1; tg.openStickers(); }
                else pressed |= item.button;
            };
            if (!ui) chat_menu_open = false;
            if (ui && (tg.currentChatId() != 0 || tg.topicsOpen()) && (pressed & SCE_CTRL_START) && !(pad.buttons & SCE_CTRL_SELECT)) {
                if (chat_menu_open) chat_menu_open = false; else open_menu();
                pressed &= ~SCE_CTRL_START;
            } else if (ui && chat_menu_open) {
                const auto items = chat_menu_items();
                const int count = (int)items.size();
                if (pressed & SCE_CTRL_UP) chat_menu_sel = (chat_menu_sel + count - 1) % count;
                if (pressed & SCE_CTRL_DOWN) chat_menu_sel = (chat_menu_sel + 1) % count;
                if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) pressed &= ~(SCE_CTRL_UP | SCE_CTRL_DOWN);
                if (pressed & SCE_CTRL_CROSS) { pressed &= ~SCE_CTRL_CROSS; run_item(items[std::min(chat_menu_sel, count - 1)]); }
                else if (pressed & SCE_CTRL_CIRCLE) { pressed &= ~SCE_CTRL_CIRCLE; chat_menu_open = false; }
                else if (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SQUARE | SCE_CTRL_TRIANGLE |
                                    SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_SELECT)) chat_menu_open = false;
            }
            if (tap && ui && !kb.active()) {
                const bool in_right = std::hypot((float)touch_x - 960.0f, (float)touch_y - 544.0f) < 66.0f;
                const bool in_left = std::hypot((float)touch_x, (float)touch_y - 544.0f) < 66.0f;
                if (in_right) {
                    if (chat_menu_open) chat_menu_open = false; else open_menu();
                    tap = false;
                } else if (in_left) {
                    chat_menu_open = false;       // the attach button takes over (handled below)
                } else if (chat_menu_open) {
                    const auto items = chat_menu_items();
                    const int top = kMenuBottom - (int)items.size() * kMenuRowHeight;
                    if (touch_x >= kMenuX && touch_x < kMenuX + kMenuWidth && touch_y >= top && touch_y < kMenuBottom)
                        run_item(items[(touch_y - top) / kMenuRowHeight]);
                    else chat_menu_open = false;
                    tap = false;
                }
            }
        }
        if (tap && !kb.active() && (circle_recorder.active() || settings_open || !tg.canBrowse() || tg.inputKind() == InputKind::None)) {
            if (circle_recorder.active()) {
                // Recorder actions live in the corner menu now; the bottom strip is not a touch target.
            } else if (qr_scanner.active()) {
                if (touch_y >= 500) pressed |= SCE_CTRL_CIRCLE;
            } else if (!settings_open && tg.topicsOpen()) {
                if (touch_y >= 500) {}
                else if (touch_y >= 70 && touch_y < 470) {
                    int index = std::max(0, topic_index-7)+(touch_y-70)/50;
                    if (index < (int)tg.topics().size()) { topic_index = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (upload_picker.active()) {
                if (upload_picker.stage == UploadPicker::Stage::Menu) {
                    if (touch_x >= 40 && touch_x < 270 && touch_y >= 366 && touch_y < 486) {
                        upload_picker.selected = (touch_y - 366) / 40; pressed |= SCE_CTRL_CROSS;
                    } else pressed |= SCE_CTRL_CIRCLE;
                } else if (upload_picker.stage == UploadPicker::Stage::Review && touch_x >= 540 && touch_y >= 109 && touch_y < 465) {
                    pressed |= SCE_CTRL_SQUARE;
                } else if (touch_y >= 500) {
                    if (upload_picker.stage == UploadPicker::Stage::Browse) {
                        pressed |= touch_x < 250 ? SCE_CTRL_SQUARE : touch_x < 500 ? SCE_CTRL_TRIANGLE : SCE_CTRL_CIRCLE;
                    } else if (touch_x >= 320 && touch_x < 640) pressed |= SCE_CTRL_TRIANGLE;
                    else if (touch_x >= 640) pressed |= SCE_CTRL_CIRCLE;
                } else if (upload_picker.stage == UploadPicker::Stage::Review && touch_y >= 117 && touch_y < 461 && touch_x < 530) {
                    int index = (upload_picker.selected / 8) * 8 + (touch_y - 117) / 43;
                    if (index < upload_picker.reviewRows()) { upload_picker.selected = index; pressed |= SCE_CTRL_CROSS; }
                } else if (upload_picker.stage == UploadPicker::Stage::Browse && touch_y >= 117 && touch_y < 461) {
                    int index = (upload_picker.selected / 8) * 8 + (touch_y - 117) / 43;
                    if (index < (int)upload_picker.entries.size()) { upload_picker.selected = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (!settings_open && stickers_open) {
                if (touch_y >= 500) {}
                else if (sticker_pack_menu && touch_y >= 116 && touch_y < 494) {
                    int index = std::max(0, sticker_pack - 6) + (touch_y - 116) / 54;
                    if (index < (int)tg.stickerPacks().size()) { sticker_pack = index; pressed |= SCE_CTRL_CROSS; }
                } else if (!sticker_pack_menu && touch_x >= 30 && touch_x < 930 && touch_y >= 130 && touch_y < 460) {
                    int index = (sticker_index/18)*18 + (touch_y-130)/110*6 + (touch_x-30)/150;
                    if (index < (int)tg.stickers().size()) { sticker_index = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (!settings_open && reactions_open) {
                if (touch_y >= 500) {}
                else if (touch_x >= 32 && touch_x < 928 && touch_y >= 130 && touch_y < 418) {
                    int index = (reaction_index/32)*32 + (touch_y-130)/72*8 + (touch_x-32)/112;
                    if (index < (int)tg.reactionChoices().size()) { reaction_index = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (video_fullscreen) {
                pressed |= SCE_CTRL_CROSS;
            } else if (!settings_open && post_open && !photo_open && message_index >= 0 && message_index < (int)tg.messages().size() && tg.messages()[message_index].poll.id) {
                if (touch_y >= 500) {}
                else if (touch_y >= 160 && touch_y < 435) {
                    int index = poll_index / 5 * 5 + (touch_y - 160) / 55;
                    if (index < (int)tg.messages()[message_index].poll.order.size()) { poll_index = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (settings_open) {
                if (folder_picker.active) {
                    if (touch_y >= 500) pressed |= touch_x < 320 ? SCE_CTRL_SQUARE :
                        touch_x < 640 ? SCE_CTRL_TRIANGLE : SCE_CTRL_CIRCLE;
                    else if (touch_y >= 117 && touch_y < 461) {
                        int index = (folder_picker.selected / 8) * 8 + (touch_y - 117) / 43;
                        if (index < (int)folder_picker.folders.size()) {
                            folder_picker.selected = index;
                            pressed |= SCE_CTRL_CROSS;
                        }
                    }
                } else if (touch_y >= 500) pressed |= SCE_CTRL_CIRCLE;
                else if (touch_y >= (proxy_open ? 104 : 88) && touch_y < (proxy_open ? 104 + 36 * (int)proxy_rows(proxy_draft.proxy_type).size() : 88 + kSettingsStride * kSettingsRows)) {
                    settings_row = proxy_open ? (touch_y - 104) / 36 : (touch_y - 88) / kSettingsStride;
                    pressed |= SCE_CTRL_CROSS;
                }
            } else if (!tg.currentChatId() && !tg.canBrowse()) {
                if ((touch_x >= 30 && touch_x < 470 && touch_y >= 280 && touch_y < 340 && tg.qrLink().empty()) ||
                    (touch_y >= 500 && touch_x >= 480)) pressed |= SCE_CTRL_START;
                else if (touch_y >= 500 && touch_x < 480) pressed |= SCE_CTRL_CROSS;
            } else if (!tg.currentChatId() && tg.canBrowse() && folder_tabs && touch_y >= 56 && touch_y < 100 && touch_x >= 20 && touch_x < 940) {
                int index = (touch_x - 20 + folder_scroll) / 180;
                if (index < (int)tg.folders().size()) { tg.selectFolder(tg.folders()[index].id); chat_index = 0; chat_scroll.reset(); }
            } else if (!tg.currentChatId() && tg.canBrowse() && touch_y >= 36 && touch_y < 54) {
                tg.loadMoreChats();
            } else if (tg.currentChatId() && tg.ready() && !post_open && !photo_open && !tg.topicsOpen() &&
                       std::hypot((float)touch_x, (float)touch_y - 544.0f) < 66.0f) {
                upload_picker.openMenu(); chat_menu_open = false;
            } else if (touch_y >= 500 && !tg.currentChatId() && !tg.canBrowse()) {
                pressed |= touch_x >= 650 ? SCE_CTRL_TRIANGLE : SCE_CTRL_CROSS;
            } else if (tg.currentChatId() && !post_open && !photo_open && touch_y >= 58 && touch_y < 544) {
                for (const auto& row : history_layout(tg.messages(), message_index, tg.currentChatIsGroup()))
                    if (touch_y >= row.top && touch_y < row.top + row.height - 4) {
                        message_index = row.index; pressed |= SCE_CTRL_CROSS; break;
                    }
            } else if (post_open && !photo_open && message_index >= 0 && message_index < (int)tg.messages().size()) {
                const auto& post = tg.messages()[message_index];
                if (scrollable_post(post)) {
                    if (touch_y >= 40 && touch_y < 544) {
                        if (post.video && touch_x >= 30 && touch_x < 930 &&
                            touch_y >= post_media_top-post_text_scroll &&
                            touch_y < post_media_top+post_media_height-post_text_scroll) {
                            pressed |= SCE_CTRL_CROSS;
                        } else if (!post.photos.empty() && touch_y >= post_photo_top-post_text_scroll && touch_y < post_photo_top+170-post_text_scroll) {
                            for (const auto& tile : photo_carousel(post, photo_index))
                                if (touch_x >= tile.x && touch_x < tile.x+tile.width) {
                                    photo_index = tile.index; pressed |= SCE_CTRL_CROSS; break;
                                }
                        } else {
                            auto lines = post_lines(font, post);
                            int relative = touch_y - (post_text_top(post)-19-post_text_scroll);
                            int line = relative / 24;
                            if (relative >= 0 && line < (int)lines.size() && !lines[line].url.empty())
                                browser_error = open_link(lines[line].url);
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
                if ((tg.messages()[message_index].voice || tg.messages()[message_index].video) && touch_x >= 30 && touch_x < 930 && touch_y >= post_media_top-post_text_scroll && touch_y < post_media_top+post_media_height-post_text_scroll)
                    pressed |= SCE_CTRL_CROSS;
                if (touch_x >= 38 && touch_x < 930 && touch_y >= text_y - 19 && touch_y < 544) {
                    auto lines = post_lines(font, tg.messages()[message_index]);
                    int line = post_text_scroll + (touch_y - (text_y - 19)) / 24;
                    if (line >= 0 && line < (int)lines.size() && !lines[line].url.empty())
                        browser_error = open_link(lines[line].url);
                }
                if (touch_x >= x0 && touch_x < (columns == 2 ? 912 : 800) && touch_y >= 90 && touch_y < text_y - 19 &&
                    row >= 0 && row < (count > 2 ? 3 : 1) && col < columns && index < count) {
                    photo_index = index; pressed |= SCE_CTRL_CROSS;
                }
                }
            } else if (!tg.currentChatId() && tg.canBrowse() && touch_y >= chat_top && touch_y < 544) {
                int index = chat_scroll.hit(touch_y - chat_top);
                if (index >= 0 && index < (int)tg.chats().size()) { chat_index = index; pressed |= SCE_CTRL_CROSS; }
            }
        }

        if (tg.currentChatId() != 0 && !tg.messages().empty()) {
            if (message_index < 0) message_index = (int)tg.messages().size() - 1;
            message_index = std::max(0, std::min(message_index, (int)tg.messages().size() - 1));
            photo_index = std::max(0, std::min(photo_index, (int)tg.messages()[message_index].photos.size() - 1));
        }

        if (!circle_recorder.active() && !settings_open && !(pressed & SCE_CTRL_START) && tg.inputKind() != InputKind::None && (tg.inputKind() == InputKind::Message || (pressed & SCE_CTRL_CROSS)) && !kb.active() && !kb.submitted() && !kb.cancelled()) {
            kb.open(tg.inputPrompt(), tg.inputSecret());
        }

        // Sign-in: step back to the phone number, or ask Telegram for the code again.
        if (!circle_recorder.active() && !settings_open && !kb.active() && !tg.canBrowse()) {
            if ((pressed & SCE_CTRL_SQUARE) && tg.canRequestQr()) { tg.requestQr(); pressed &= ~SCE_CTRL_SQUARE; }
            else if ((pressed & SCE_CTRL_CIRCLE) && !tg.qrLink().empty()) {
                // TDLib cannot return from the QR state to a phone number, so the sign-in data is
                // cleared and the app restarts at the phone prompt (the same path as after a logout).
                if (account_cleanup::mark()) { tg.close(); restart_requested = true; running = false; }
                pressed &= ~SCE_CTRL_CIRCLE;
            }
            else if ((pressed & SCE_CTRL_CIRCLE) && tg.canGoBackInput()) { tg.backInput(); pressed &= ~SCE_CTRL_CIRCLE; }
            else if ((pressed & SCE_CTRL_TRIANGLE) && tg.canResendCode()) { tg.resendCode(); pressed &= ~SCE_CTRL_TRIANGLE; }
        }

        bool toggle_voice = false;
        bool toggle_video = false;
        bool record_toggle = false;

        if (unsupported_video_prompt) {
            if (pressed & SCE_CTRL_CROSS) {
                portrait_preparation_requested = true;
                portrait_preparation_chat = unsupported_video_chat;
                portrait_preparation_message = unsupported_video_message;
                unsupported_video_prompt = false;
                toggle_video = true;
            } else if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE)) unsupported_video_prompt = false;
            pressed = 0;
        }
        if ((pressed & SCE_CTRL_START) && !(pad.buttons & SCE_CTRL_SELECT) && !kb.active() && !qr_scanner.active() && !circle_recorder.active() && !tg.currentChatId()) {
            bool auth_proxy = !settings_open && !tg.canBrowse();
            settings_open = !settings_open;
            proxy_open = auth_proxy;
            proxy_return_to_screen = auth_proxy;
            proxy_imported = false;
            if (auth_proxy) proxy_draft = settings;
            settings_error.clear();
            settings_row = 0; confirm_logout = false; confirm_vault_off = false; pressed = 0;
        }
        if (kb.active()) {
            kb.handle(pressed);
        } else if (kb.submitted()) {
            std::string v = kb.text();
            kb.close();
            if (folder_name_input) {
                folder_name_input = false;
                folder_picker.create(v);
            } else if (proxy_input >= 0) {
                if (proxy_input == PROXY_ROW_SERVER) proxy_draft.proxy_server = v;
                else if (proxy_input == PROXY_ROW_SECRET) proxy_draft.proxy_secret = v;
                else if (proxy_input == PROXY_ROW_USER) proxy_draft.proxy_username = v.substr(0, 255);
                else if (proxy_input == PROXY_ROW_PASS) proxy_draft.proxy_password = v.substr(0, 255);
                else {
                    int port = 0;
                    bool valid = !v.empty() && v.size() <= 5;
                    for (char c : v) { if (!valid || c < '0' || c > '9') { valid = false; break; } port = port * 10 + c - '0'; }
                    if (valid && port > 0 && port <= 65535) proxy_draft.proxy_port = port;
                    else settings_error = tr("proxy.err.port_range");
                }
                proxy_input = -1;
            } else tg.submitInput(v);
        } else if (kb.cancelled()) {
            kb.close();
            if (folder_name_input) folder_name_input = false;
            else if (proxy_input >= 0) proxy_input = -1; else tg.cancelInput();
        } else if (circle_recorder.active()) {
            using RecordingState = VideoNoteRecorder::State;
            auto state = circle_recorder.state();
            if (pressed & SCE_CTRL_CIRCLE) {
                circle_playback.reset(); circle_recorder.cancel(); circle_error.clear();
            } else {
                if (pressed & SCE_CTRL_TRIANGLE) circle_recorder.switchCamera();
                if ((pressed & SCE_CTRL_SQUARE) && state == RecordingState::Review && circle_playback) circle_playback->toggle();
                if (pressed & SCE_CTRL_CROSS) {
                    if (state == RecordingState::Preview) circle_recorder.record();
                    else if (state == RecordingState::Recording) circle_recorder.finish();
                    else if (state == RecordingState::Review) {
                        if (tg.sendVideoNoteFile(circle_recorder.path(), circle_recorder.seconds(), circle_chat, circle_topic)) {
                            circle_playback.reset(); circle_recorder.releaseFile(); circle_error.clear();
                        } else circle_error = tg.videoNoteStatus();
                    } else if (state == RecordingState::Error) {
                        circle_playback.reset(); circle_recorder.cancel(); open_circle();
                    }
                }
            }
        } else if ((recorder.recording() || recorder.processing()) &&
                   (pressed & (SCE_CTRL_SQUARE | SCE_CTRL_CIRCLE))) {
            recorder.cancel();
            recorder.setStatus(tr("common.cancelled"));
            audio_chat = 0; audio_topic = 0;
            pressed &= ~(SCE_CTRL_SQUARE | SCE_CTRL_CIRCLE);
        } else if (qr_scanner.active()) {
            if (pressed & SCE_CTRL_CIRCLE) { qr_scanner.stop(); settings_row = 5; }
        } else if (!settings_open && tg.topicsOpen()) {
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
        } else if (upload_picker.active()) {
            using Stage = UploadPicker::Stage;
            const int rows = upload_picker.stage == Stage::Menu ? 3 : upload_picker.stage == Stage::Review ? upload_picker.reviewRows() : (int)upload_picker.entries.size();
            if (rows > 0) {
                if (pressed & SCE_CTRL_UP) upload_picker.selected = (upload_picker.selected + rows - 1) % rows;
                if (pressed & SCE_CTRL_DOWN) upload_picker.selected = (upload_picker.selected + 1) % rows;
                if (upload_picker.stage == Stage::Browse) {
                    if (pressed & SCE_CTRL_LTRIGGER) upload_picker.selected = std::max(0, upload_picker.selected - 8);
                    if (pressed & SCE_CTRL_RTRIGGER) upload_picker.selected = std::min(rows - 1, upload_picker.selected + 8);
                }
            }
            if (upload_picker.stage == Stage::Menu) {
                if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) upload_picker.close();
                else if (pressed & SCE_CTRL_CROSS) upload_picker.chooseKind((UploadPicker::Kind)upload_picker.selected);
            } else if (upload_picker.stage == Stage::Browse) {
                if (pressed & SCE_CTRL_CIRCLE) upload_picker.up();
                else if (pressed & SCE_CTRL_TRIANGLE) upload_picker.refresh();
                else if (pressed & SCE_CTRL_SQUARE) upload_picker.toggleHidden();
                else if (pressed & SCE_CTRL_CROSS) upload_picker.enter();
            } else {
                const auto* focus = upload_picker.focusedItem();
                if ((pressed & SCE_CTRL_SQUARE) && focus && focus->kind == UploadPicker::Kind::Video) toggle_video = true;
                if (pressed & SCE_CTRL_CIRCLE) upload_picker.close();
                else if (pressed & SCE_CTRL_TRIANGLE) upload_picker.removeSelected();
                else if (pressed & SCE_CTRL_CROSS) upload_picker.reviewActivate();
            }
        } else if (!settings_open && stickers_open) {
            if (sticker_pack_menu) {
                if (pressed & SCE_CTRL_CIRCLE) stickers_open = false;
                int count = (int)tg.stickerPacks().size();
                if (pressed & SCE_CTRL_UP) --sticker_pack;
                if (pressed & SCE_CTRL_DOWN) ++sticker_pack;
                sticker_pack = std::max(0, std::min(std::max(0, count - 1), sticker_pack));
                if ((pressed & SCE_CTRL_CROSS) && count) {
                    sticker_pack_menu = false; sticker_index = 0; tg.selectStickerPack(sticker_pack);
                }
            } else {
                if (pressed & SCE_CTRL_SQUARE) stickers_open = false;
                if (pressed & SCE_CTRL_CIRCLE) { sticker_pack_menu = true; tg.loadStickerPacks(); }
                int count = (int)tg.stickers().size();
                if (pressed & SCE_CTRL_LEFT) --sticker_index;
                if (pressed & SCE_CTRL_RIGHT) ++sticker_index;
                if (pressed & SCE_CTRL_UP) sticker_index -= 6;
                if (pressed & SCE_CTRL_DOWN) sticker_index += 6;
                sticker_index = std::max(0, std::min(std::max(0, count-1), sticker_index));
                if ((pressed & SCE_CTRL_CROSS) && tg.sendSticker(sticker_index)) stickers_open = false;
            }
        } else if (!settings_open && reactions_open) {
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
        } else if (settings_open && folder_picker.active) {
            if ((pressed & SCE_CTRL_TRIANGLE) && !folder_picker.path.empty()) {
                folder_name_input = true;
                kb.open(tr("picker.new_folder_title"));
            }
            if (pressed & SCE_CTRL_CIRCLE) folder_picker.up();
            if (pressed & SCE_CTRL_UP && !folder_picker.folders.empty())
                folder_picker.selected = (folder_picker.selected + (int)folder_picker.folders.size() - 1) % (int)folder_picker.folders.size();
            if (pressed & SCE_CTRL_DOWN && !folder_picker.folders.empty())
                folder_picker.selected = (folder_picker.selected + 1) % (int)folder_picker.folders.size();
            if (pressed & SCE_CTRL_CROSS) folder_picker.enter();
            if ((pressed & SCE_CTRL_SQUARE) && !folder_picker.path.empty()) {
                settings.download_dir = folder_picker.path;
                settings_error = settings.save() ? "" : tr("settings.save_failed");
                folder_picker.active = false;
            }
        } else if (settings_open && proxy_open) {
            if (pressed & SCE_CTRL_CIRCLE) {
                tg.cancelProxyCheck(); proxy_open = false; settings_row = 8; settings_error.clear(); proxy_imported = false;
                if (proxy_return_to_screen) { settings_open = false; proxy_return_to_screen = false; }
            }
            else {
                const std::vector<int> rows = proxy_rows(proxy_draft.proxy_type);
                const int count = (int)rows.size();
                if (settings_row >= count) settings_row = count - 1;
                const int row = rows[settings_row];
                if (pressed & SCE_CTRL_UP) settings_row = (settings_row + count - 1) % count;
                if (pressed & SCE_CTRL_DOWN) settings_row = (settings_row + 1) % count;
                if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT) && row == PROXY_ROW_TYPE) {
                    proxy_draft.proxy_type = (proxy_draft.proxy_type + 3 + ((pressed & SCE_CTRL_LEFT) ? -1 : 1)) % 3;
                    settings_error.clear(); tg.cancelProxyCheck();
                }
                if (pressed & SCE_CTRL_CROSS) {
                    settings_error.clear();
                    if (row == PROXY_ROW_USE) proxy_draft.proxy_enabled = !proxy_draft.proxy_enabled;
                    else if (row == PROXY_ROW_TYPE) {
                        proxy_draft.proxy_type = (proxy_draft.proxy_type + 1) % 3; tg.cancelProxyCheck();
                    } else if (row >= PROXY_ROW_SERVER && row <= PROXY_ROW_PASS) {
                        tg.cancelProxyCheck(); proxy_input = row;
                        const bool secret = row == PROXY_ROW_SECRET || row == PROXY_ROW_PASS;
                        const char* title = row == PROXY_ROW_SERVER ? "proxy.kb.server" : row == PROXY_ROW_PORT ? "proxy.kb.port" :
                            row == PROXY_ROW_SECRET ? "proxy.kb.secret" : row == PROXY_ROW_USER ? "proxy.kb.username" : "proxy.kb.password";
                        kb.open(tr(title), secret, row == PROXY_ROW_SERVER ? proxy_draft.proxy_server : row == PROXY_ROW_PORT ?
                            std::to_string(proxy_draft.proxy_port) : row == PROXY_ROW_SECRET ? proxy_draft.proxy_secret :
                            row == PROXY_ROW_USER ? proxy_draft.proxy_username : proxy_draft.proxy_password);
                    } else if (row == PROXY_ROW_CHECK) {
                        tg.checkProxy(proxy_draft.proxyConfig());
                    } else if (row == PROXY_ROW_SCAN) {
                        tg.cancelProxyCheck();
                        if (!qr_scanner.start()) settings_error = qr_scanner.status();
                    } else if (proxy_draft.proxy_enabled && !proxy_draft.proxyConfig().complete()) {
                        settings_error = tr("proxy.need_fields");
                    } else if (!proxy_draft.save()) settings_error = tr("settings.save_failed");
                    else {
                        settings = proxy_draft;
                        proxy_imported = false;
                        tg.configureProxy(settings.proxy_enabled, settings.proxyConfig());
                    }
                }
            }
        } else if (settings_open) {
            if (pressed & SCE_CTRL_CIRCLE) { if (confirm_logout) confirm_logout = false; else settings_open = false; }
            if (pressed & SCE_CTRL_UP) { settings_row = (settings_row+kSettingsRows-1)%kSettingsRows; confirm_logout = false; confirm_vault_off = false; }
            if (pressed & SCE_CTRL_DOWN) { settings_row = (settings_row+1)%kSettingsRows; confirm_logout = false; confirm_vault_off = false; }
            if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) {
                int direction = (pressed & SCE_CTRL_LEFT) ? -1 : 1;
                // Row 0 is the language; the remaining rows keep their original order.
                const int row = settings_row - 1;
                if (settings_row == 0) {
                    confirm_logout = false;
                    // Automatic first, then every language file found in app0:lang.
                    std::vector<std::string> choices{""};
                    for (const auto& language : i18n::languages()) choices.push_back(language.code);
                    size_t at = std::find(choices.begin(), choices.end(), settings.language) - choices.begin();
                    if (at >= choices.size()) at = 0;
                    settings.language = choices[(at + choices.size() + direction) % choices.size()];
                    i18n::select(settings.language);
                    apply_language_font();
                    settings_error = settings.save() ? "" : tr("settings.save_failed");
                } else if (row < 4) {
                    confirm_logout = false;
                    int* values[] = {&settings.retention, &settings.capacity, &settings.theme, &settings.font};
                    const int counts[] = {5, 6, 2, 7};
                    *values[row] = (*values[row] + counts[row] + direction) % counts[row];
                    rich_text_set_scale(settings.fontScale());
                    settings_error = settings.save() ? "" : tr("settings.save_failed");
                    next_cache_cleanup = now + 300000000;
                } else if (row == 4) {
                    settings.show_threads = !settings.show_threads;
                    settings_error = settings.save() ? "" : tr("settings.save_failed");
                } else if (row == 5) {
                    int index = (AppSettings::requestBatchIndex(settings.request_batch_size) + direction + 5) % 5;
                    settings.request_batch_size = AppSettings::requestBatchSize(index);
                    tg.setRequestBatchSize(settings.request_batch_size);
                    settings_error = settings.save() ? "" : tr("settings.save_failed");
                } else if (row == 6) {
                    if (pressed & SCE_CTRL_CROSS) folder_picker.open(settings.download_dir);
                } else if (row == 7) {
                    if (pressed & SCE_CTRL_CROSS) { proxy_open = true; proxy_return_to_screen = proxy_imported = false; proxy_draft = settings; settings_row = 0; settings_error.clear(); confirm_logout = false; }
                } else if (row == 8) {
                    confirm_logout = false;
                    settings.check_updates = !settings.check_updates;
                    settings_error = settings.save() ? "" : tr("settings.save_failed");
                } else if (row == 9) {
                    confirm_logout = false;
                    if (pressed & SCE_CTRL_CROSS) {
                        const Updater::Snapshot u = updater.snapshot();
                        settings_error.clear();
                        if (u.state == Updater::State::Available) updater.install();
                        else if (u.busy() && u.state != Updater::State::Installing) updater.cancel();
                        else if (u.state == Updater::State::Idle || u.state == Updater::State::UpToDate || u.state == Updater::State::Failed) updater.check();
                    }
                } else if (row == 10) {
                    // Data encryption: switching on/off restarts the app, because existing files are
                    // converted before TDLib starts (see vault_ui::run_migration).
                    confirm_logout = false;
                    if (pressed & SCE_CTRL_CROSS) {
                        const vault_ui::Env vault_env{font, (settings.theme == 1)};
                        auto finish_vault_flow = [&](vault_ui::Outcome outcome, bool restart) {
                            // Buttons used in the dialog must not leak into the main screen.
                            SceCtrlData after{}; sceCtrlPeekBufferPositive(0, &after, 1); old_buttons = after.buttons; pressed = 0;
                            vita2d_set_clear_color((settings.theme == 1) ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255));
                            if (outcome == vault_ui::Outcome::Failed) settings_error = tr("vault.error");
                            else if (outcome == vault_ui::Outcome::Done) {
                                settings_error = settings.save() ? "" : tr("settings.save_failed");
                                if (restart) { restart_requested = true; tg.close(); settings_open = false; running = false; }
                            }
                        };
                        if (vault::disabling()) settings_error = tr("vault.settings.pending");
                        else if (!vault::enabled()) { confirm_vault_off = false; finish_vault_flow(vault_ui::enable_flow(vault_env), true); }
                        else if (!confirm_vault_off) confirm_vault_off = true;
                        else { confirm_vault_off = false; finish_vault_flow(vault_ui::disable_flow(vault_env), true); }
                    }
                } else if (row == 11) {
                    confirm_logout = false; confirm_vault_off = false;
                    if (pressed & SCE_CTRL_CROSS) {
                        if (!vault::enabled() || vault::disabling()) settings_error = tr("vault.settings.not_enabled");
                        else {
                            const vault_ui::Env vault_env{font, (settings.theme == 1)};
                            auto outcome = vault_ui::change_flow(vault_env);
                            SceCtrlData after{}; sceCtrlPeekBufferPositive(0, &after, 1); old_buttons = after.buttons; pressed = 0;
                            vita2d_set_clear_color((settings.theme == 1) ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255));
                            settings_error = outcome == vault_ui::Outcome::Done ? tr("vault.changed") :
                                outcome == vault_ui::Outcome::Failed ? tr("vault.error") : "";
                        }
                    }
                } else if (row == 12) {
                    confirm_logout = false; confirm_vault_off = false;
                    if (pressed & SCE_CTRL_CROSS) {
                        if (!vault::enabled() || vault::disabling()) settings_error = tr("vault.settings.not_enabled");
                        else {
                            vault_ui::check_flow(vault_ui::Env{font, settings.theme == 1});
                            SceCtrlData after{}; sceCtrlPeekBufferPositive(0, &after, 1); old_buttons = after.buttons; pressed = 0;
                            vita2d_set_clear_color(settings.theme == 1 ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255));
                            settings_error.clear();
                        }
                    }
                } else if (pressed & SCE_CTRL_CROSS) {
                    if (!tg.ready()) settings_error = tr("settings.logout_wait");
                    else if (confirm_logout) { tg.logout(); confirm_logout = false; settings_open = false; }
                    else confirm_logout = true;
                }
            }
        } else if (tg.currentChatId() != 0) {
            const auto& messages = tg.messages();
            if (video_fullscreen) {
                if (pressed & (SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE)) video_fullscreen = false;
                if ((pressed & SCE_CTRL_CROSS) && post_open && message_index >= 0 &&
                    message_index < (int)messages.size() && messages[message_index].video) toggle_video = true;
            } else if (photo_open) {
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
            } else if (post_open && message_index >= 0 && message_index < (int)messages.size() && messages[message_index].poll.id) {
                const auto& m = messages[message_index]; const auto& poll = m.poll;
                std::string signature;
                for (const auto& option : poll.options) signature += std::to_string(option.text.size()) + ":" + option.text;
                if (poll_message != m.id || poll_chat != tg.currentChatId() || poll_signature != signature) {
                    poll_signature = signature;
                    poll_message = m.id; poll_chat = tg.currentChatId(); poll_index = 0; poll_choices.clear();
                    for (int i = 0; i < (int)poll.options.size(); ++i) if (poll.options[i].chosen) poll_choices.push_back(i);
                }
                if (pressed & SCE_CTRL_CIRCLE) { post_open = false; poll_message = 0; }
                if (pressed & SCE_CTRL_UP) --poll_index;
                if (pressed & SCE_CTRL_DOWN) ++poll_index;
                poll_index = std::max(0, std::min(poll_index, (int)poll.order.size()-1));
                if (post_open && poll.canVote() && !tg.pollPending(m.id)) {
                    if ((pressed & SCE_CTRL_CROSS) && !poll.order.empty()) {
                        int option = poll.order[poll_index];
                        auto found = std::find(poll_choices.begin(), poll_choices.end(), option);
                        if (found != poll_choices.end()) poll_choices.erase(found);
                        else { if (!poll.multiple) poll_choices.clear(); poll_choices.push_back(option); }
                    }
                    if ((pressed & SCE_CTRL_TRIANGLE) && !poll_choices.empty()) tg.answerPoll(m.id, poll_choices);
                    if ((pressed & SCE_CTRL_SQUARE) && tg.answerPoll(m.id, {})) poll_choices.clear();
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
                        video_fullscreen = false;
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
                    if ((pressed & SCE_CTRL_CROSS) && messages[message_index].document)
                        tg.downloadDocument(messages[message_index], settings.download_dir);
                    if (scrollable_post(messages[message_index])) {
                        if (pressed & SCE_CTRL_UP) post_text_scroll -= 24;
                        if (pressed & SCE_CTRL_DOWN) post_text_scroll += 24;
                        post_text_scroll = std::max(0, std::min(post_text_scroll, post_scroll_max(messages[message_index], lines.size())));
                    } else {
                    if ((pressed & SCE_CTRL_UP) && post_text_scroll > 0) post_text_scroll--;
                    if ((pressed & SCE_CTRL_DOWN) && post_text_scroll + 1 < (int)lines.size()) post_text_scroll++;
                    }
                    int link_line = scrollable_post(messages[message_index]) ? std::max(0, (post_text_scroll-post_text_top(messages[message_index])+62)/24) : post_text_scroll;
                    if (pressed & SCE_CTRL_TRIANGLE) {
                        if (messages[message_index].video && !messages[message_index].video_note) video_fullscreen = true;
                        else if (link_line < (int)lines.size() && !lines[link_line].url.empty())
                            browser_error = open_link(lines[link_line].url);
                    }
                    if ((pressed & SCE_CTRL_CROSS) && count) {
                        tg.ensureFullPhoto((size_t)message_index, (size_t)photo_index);
                        photo_open = true;
                        photo_view.reset();
                    }
                }
            } else {
                if (pressed & SCE_CTRL_CIRCLE) { if (tg.inTopic()) tg.backToTopics(); else tg.leaveChat(); pressed = 0; }
                if (pressed & SCE_CTRL_SQUARE) tg.requestCompose();
                if ((pressed & SCE_CTRL_SELECT) && !(pad.buttons & SCE_CTRL_START) && tg.ready()) { upload_picker.openMenu(); chat_menu_open = false; }
                if (pressed & SCE_CTRL_LTRIGGER) open_circle();
                else if (pressed & SCE_CTRL_RTRIGGER) record_toggle = true;
                if (pressed & SCE_CTRL_TRIANGLE) tg.refreshHistory();
                if (pressed & SCE_CTRL_UP) {
                    if (message_index > 0) message_index--;
                    if (message_index <= 2) tg.loadOlder();
                }
                if ((pressed & SCE_CTRL_DOWN) && message_index + 1 < (int)messages.size()) message_index++;
                if ((pressed & SCE_CTRL_CROSS) && !messages.empty() && message_index >= 0) {
                    if (messages[message_index].pin_message)
                        tg.openPinnedMessage(messages[message_index].pinned_message_id);
                    else if (!messages[message_index].service_message) post_open = true;
                    video_fullscreen = false;
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
                tg.selectFolder(tg.folders()[next].id); chat_index = 0; chat_scroll.reset();
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
                if ((pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) || (!tap && (pressed & SCE_CTRL_CROSS)))
                    chat_scroll.reveal(chat_index);
                if (pressed & SCE_CTRL_CROSS) {
                    tg.openChat(chats[chat_index].id, chats[chat_index].title);
                    topic_index = 0;
                    message_index = -1;
                    post_open = photo_open = false;
                }
            }
            if (pressed & SCE_CTRL_TRIANGLE) tg.refreshChats();
        }

        if (record_toggle && !circle_recorder.active() && tg.currentChatId() && !kb.active() && !post_open && !photo_open) {
            if (recorder.recording()) {
                recorder.stop();
            } else if (!recorder.processing()) {
                audio_chat = tg.currentChatId(); audio_topic = tg.currentTopicId();
                recorder.start("ux0:data/vitagram/voice-message-" + std::to_string(now) + ".ogg");
            }
        }
        std::string completed_audio = recorder.takeCompleted();
        if (!completed_audio.empty()) {
            const bool sent = tg.sendAudioFile(completed_audio, recorder.seconds(), audio_chat, audio_topic);
            // Upload tracking now owns the progress/result, not the recorder.
            recorder.setStatus(sent ? "" : tr("rec.send_failed"));
        }

        std::vector<UploadPicker::Item> upload_batch;
        if (upload_picker.takeResult(upload_batch)) {
            // Photos and videos go compressed in one album message; other files stay untouched documents.
            std::vector<std::pair<std::string, int>> files;
            for (const auto& item : upload_batch) files.emplace_back(item.path, (int)item.kind);
            if (!tg.sendLocalFiles(files, tg.currentChatId(), tg.currentTopicId()))
                recorder.setStatus(tr("upload.send_failed"));
        }

        if (exit_gesture.update(pad.buttons & SCE_CTRL_START, pad.buttons & SCE_CTRL_SELECT,
                pressed & SCE_CTRL_SELECT, kb.active(), now)) {
            tg.close();
            running = false;
        }

        input_actions.finish();
        performance::SlowScope media_update(performance::SlowPhase::Ui,"media_update");
        if (qr_scanner.active()) {
            std::string scanned = qr_scanner.takeLink();
            if (!scanned.empty()) { pending_proxy_link = scanned; qr_scanner.stop(); }
            else qr_scanner.upload();
        }
        if (!pending_proxy_link.empty()) {
            ProxyLink proxy; std::string error;
            auto parsed = parse_proxy_link(pending_proxy_link, proxy, error);
            pending_proxy_link.clear();
            if (parsed == ProxyLinkResult::Valid) {
                proxy_return_to_screen = proxy_return_to_screen || !settings_open;
                if (!settings_open || !proxy_open) proxy_draft = settings;
                proxy_draft.proxy_server = proxy.server; proxy_draft.proxy_port = proxy.port;
                proxy_draft.proxy_secret = proxy.secret; proxy_draft.proxy_enabled = true;
                proxy_draft.proxy_type = (int)proxy.kind;
                proxy_draft.proxy_username = proxy.username; proxy_draft.proxy_password = proxy.password;
                settings_open = proxy_open = proxy_imported = true;
                {
                    const auto rows = proxy_rows(proxy_draft.proxy_type);
                    settings_row = (int)(std::find(rows.begin(), rows.end(), (int)PROXY_ROW_SAVE) - rows.begin());
                } settings_error.clear(); confirm_logout = false;
            } else {
                if (settings_open) settings_error = error;
                else browser_error = error;
            }
        }

        if (status_chat != tg.currentChatId() || status_topic != tg.currentTopicId()) {
            status_chat = tg.currentChatId(); status_topic = tg.currentTopicId();
            browser_error.clear(); browser_notice.clear(); audio_notice.clear();
            tg.dismissStatusNotices();
            if (!recorder.recording() && !recorder.processing()) recorder.setStatus("");
        }
        const std::string browser_status = browser_notice.update(browser_error, now);
        const bool audio_context = audio_chat == tg.currentChatId() && audio_topic == tg.currentTopicId();
        const std::string recorder_status = audio_notice.update(
            !recorder.recording() && !recorder.processing() && (audio_context || !audio_chat) ? recorder.status() : "", now);

        if (circle_recorder.active()) {
            circle_recorder.upload();
            if (circle_recorder.state() == VideoNoteRecorder::State::Review) {
                if (!circle_playback) {
                    circle_playback.reset(new AnimationPlayer());
                    circle_playback->setSource(circle_recorder.path(), true, true);
                }
                circle_playback->upload();
            }
        }

        if (!settings_open && post_open && !photo_open && tg.currentChatId() && message_index>=0 && message_index<(int)tg.messages().size())
            tg.viewMessage(tg.messages()[message_index].id);
        int animation_index = !circle_recorder.active() && !settings_open && post_open && !photo_open && !kb.active() && running &&
            message_index >= 0 && message_index < (int)tg.messages().size() && tg.messages()[message_index].animation ? message_index : -1;
        tg.setAnimationTarget(animation_index);
        const MessageItem* animation_message = animation_index >= 0 ? &tg.messages()[animation_index] : nullptr;
        const bool unsupported_video = animation_message && animation_message->video && !animation_message->video_note &&
            animation_message->video_width > 0 && animation_message->video_height > 0 &&
            !VideoQuality::hardwareRasterSupported(animation_message->video_width, animation_message->video_height);
        const bool prepare_video = unsupported_video && portrait_preparation_requested &&
            tg.currentChatId() == portrait_preparation_chat && animation_message->id == portrait_preparation_message;
        if (portrait_preparation_requested && !prepare_video) portrait_preparation_requested = false;
        if (unsupported_video_prompt && (!animation_message || tg.currentChatId() != unsupported_video_chat ||
            animation_message->id != unsupported_video_message)) unsupported_video_prompt = false;
        if (unsupported_video && !prepare_video && (toggle_video || video_fullscreen)) {
            video_fullscreen = false;
            toggle_video = false;
            unsupported_video_prompt = true;
            unsupported_video_chat = tg.currentChatId();
            unsupported_video_message = animation_message->id;
        }
        const auto* picker_focus = upload_picker.stage == UploadPicker::Stage::Review ? upload_picker.focusedItem() : nullptr;
        const bool picker_video = picker_focus && picker_focus->kind == UploadPicker::Kind::Video;
        if (picker_video) animation.setSource(picker_focus->path, true, false, false);
        else animation.setSource(animation_message && animation_message->animation_error.empty() && (!unsupported_video || prepare_video) ? animation_message->animation_path : "",
            animation_message && animation_message->video, animation_message && animation_message->video_note, prepare_video);
        if (toggle_video && (animation_message || picker_video)) animation.toggle();
        animation.upload();
        int voice_index = !circle_recorder.active() && !settings_open && post_open && !photo_open && !kb.active() && running && tg.currentChatId() &&
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

        media_update.finish();
        performance::SlowScope draw_stage(performance::SlowPhase::Ui,"draw");
        performance::SlowScope draw_begin(performance::SlowPhase::Ui,"draw_begin");
        vita2d_start_drawing();
        vita2d_clear_screen();
        draw_begin.finish();
        performance::SlowScope draw_header(performance::SlowPhase::Ui,"draw_header");

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
        std::string heading = settings_open ? (proxy_open ? tr("proxy.title") : tr("settings.title")) : tg.currentChatId() ? tg.currentChatTitle() : "VitaGram " VITAGRAM_VERSION;
        std::string presence = settings_open ? "" : tg.currentUserStatus();
        int status_width = presence.empty() ? 280 : 390;
        draw_text(font, title_x, 25, fit_text(font, heading, 920-status_width-title_x, 0.65f), header_text, 0.65f);
        std::string network = fit_text(font, presence.empty() ? (net_ok ? net.status() : net.error()) : presence, status_width, 0.52f);
        draw_text(font, 940-rich_text_width(font, 0.52f, network.c_str()), 25, network, header_text, 0.52f);
        std::string update_notice = tg.updateNotice();


        if (qr_scanner.active()) {
            if (qr_scanner.preview()) draw_texture_fit(qr_scanner.preview(), 240, 60, 480, 360);
            draw_text(font, 30, 462, fit_text(font, qr_scanner.status(), 900, 0.55f), ink, 0.55f);
            draw_text(font, 28, 528, tr("qr.footer"), ink, 0.6f);
        } else if (settings_open && folder_picker.active) {
            draw_text(font, 30, 68, fit_text(font, folder_picker.path.empty() ? tr("picker.drives") : folder_picker.path, 900, 0.64f), ink, 0.64f);
            draw_text(font, 32, 97, tr("picker.subtitle"), muted, 0.48f);
            rounded_rect(24, 109, 912, 356, separator);
            rounded_rect(26, 111, 908, 352, white);
            int first = (folder_picker.selected / 8) * 8;
            for (int row = 0; row < 8 && first + row < (int)folder_picker.folders.size(); ++row) {
                int index = first + row;
                if (index == folder_picker.selected) rounded_rect(32, 117 + row * 43, 896, 40, selected);
                draw_text(font, 42, 144 + row * 43, fit_text(font, folder_picker.folders[index], 850, 0.65f), ink, 0.65f);
            }
            if (folder_picker.folders.empty() && folder_picker.error.empty())
                draw_text(font, 42, 144, tr("picker.empty"), muted, 0.55f);
            if (!folder_picker.error.empty()) draw_text(font, 30, 484, folder_picker.error, ink, 0.55f);
            draw_text(font, 28, 528, tr("picker.footer"), ink, 0.6f);
        } else if (settings_open && proxy_open) {
            const std::vector<int> rows = proxy_rows(proxy_draft.proxy_type);
            auto label = [&](int row) -> std::string {
                switch (row) {
                    case PROXY_ROW_USE: return tr("proxy.use");
                    case PROXY_ROW_TYPE: return tr("proxy.type");
                    case PROXY_ROW_SERVER: return tr("proxy.server");
                    case PROXY_ROW_PORT: return tr("proxy.port");
                    case PROXY_ROW_SECRET: return "Secret";
                    case PROXY_ROW_USER: return tr("proxy.username");
                    case PROXY_ROW_PASS: return tr("proxy.password");
                    case PROXY_ROW_SAVE: return tr("proxy.save_apply");
                    case PROXY_ROW_SCAN: return tr("proxy.scan_qr");
                    default: return tr("proxy.check");
                }
            };
            auto value = [&](int row) -> std::string {
                switch (row) {
                    case PROXY_ROW_USE: return proxy_draft.proxy_enabled ? tr("common.yes") : tr("common.no");
                    case PROXY_ROW_TYPE: return tr(proxy_draft.proxy_type == 1 ? "proxy.type.socks5" :
                        proxy_draft.proxy_type == 2 ? "proxy.type.https" : "proxy.type.mtproto");
                    case PROXY_ROW_SERVER: return clip(proxy_draft.proxy_server, 30);
                    case PROXY_ROW_PORT: return std::to_string(proxy_draft.proxy_port);
                    case PROXY_ROW_SECRET: return proxy_draft.proxy_secret.empty() ? tr("common.not_set") : std::string("********");
                    case PROXY_ROW_USER: return proxy_draft.proxy_username.empty() ? tr("common.not_set") : clip(proxy_draft.proxy_username, 30);
                    case PROXY_ROW_PASS: return proxy_draft.proxy_password.empty() ? tr("common.not_set") : std::string("********");
                    case PROXY_ROW_SCAN: return tr("vnr.rear_camera");
                    default: return "";
                }
            };
            for (int i = 0; i < (int)rows.size(); ++i) {
                rounded_rect(24, 104+i*36, 912, 32, i == settings_row ? selected : white);
                draw_text(font, 40, 128+i*36, label(rows[i]), ink, 0.62f);
                draw_text(font, 520, 128+i*36, value(rows[i]), ink, 0.58f);
            }
            draw_text(font, 30, 438, proxy_imported ? tr("proxy.import_confirm") : "", muted, 0.48f);
            draw_text(font, 30, 462, fit_text(font, tg.proxyCheckStatus().empty() ? tg.proxyStatus() : tg.proxyCheckStatus(), 900, 0.55f), muted, 0.55f);
            draw_text(font, 30, 484, settings_error, ink, 0.5f);
            draw_text(font, 28, 528, tr("proxy.footer"), ink, 0.6f);
        } else if (settings_open) {
            const std::string labels[] = {tr("settings.language"), tr("settings.retention"), tr("settings.cache_size"), tr("settings.theme"),
                tr("settings.font_size"), tr("settings.show_threads"), tr("settings.batch"), tr("settings.download_dir"), tr("proxy.title"),
                tr("settings.auto_update"), tr("settings.update"), tr("vault.settings.encryption"), tr("vault.settings.change_key"), tr("vault.settings.check"), tr("settings.logout")};
            std::string values[] = {
                language_label(),
                settings.retention == 4 ? tr("settings.always") : tr("settings.days", {std::to_string(AppSettings::days(settings.retention))}),
                settings.capacity == 5 ? tr("settings.no_limit") : tr("unit.mb", {std::to_string(AppSettings::megabytes(settings.capacity))}),
                dark ? tr("settings.theme_dark") : tr("settings.theme_light"), std::to_string(70+settings.font*10) + "%",
                settings.show_threads ? tr("common.yes") : tr("common.no"), std::to_string(settings.request_batch_size),
                settings.download_dir, settings.proxy_enabled ? tr("common.enabled") : tr("common.disabled"),
                settings.check_updates ? tr("common.yes") : tr("common.no"), update_label(updater.snapshot()),
                confirm_vault_off ? tr("vault.settings.confirm_disable") :
                    vault::disabling() ? tr("vault.settings.disabling") :
                    vault::enabled() ? tr("common.enabled") : tr("common.disabled"),
                vault::enabled() && !vault::disabling() ? "" : "-",
                vault::enabled() && !vault::disabling() ? "" : "-",
                confirm_logout ? tr("settings.confirm_logout") : ""};
            for (int row=0; row<kSettingsRows; ++row) {
                rounded_rect(24, 88+row*kSettingsStride, 912, kSettingsStride-2, row == settings_row ? selected : white);
                draw_text(font, 40, 88+row*kSettingsStride+18, labels[row], ink, 0.58f);
                draw_text(font, 520, 88+row*kSettingsStride+18, fit_text(font, values[row], 400, 0.54f), ink, 0.54f);
            }
            { const Updater::Snapshot u = updater.snapshot();
              draw_text(font, 30, 484, fit_text(font, !settings_error.empty() ? settings_error : u.state == Updater::State::Failed ? u.detail : std::string(), 900, 0.5f), ink, 0.5f); }
            draw_text(font, 28, 528, tr("settings.footer"), ink, 0.6f);
        } else if (!net_ok) {
            draw_text(font, 30, 115, tr("net.init_failed"), ink, 0.85f);
        } else if (!tg.lastError().empty() && !tg.ready()) {
            draw_text(font, 30, 115, clip(tg.lastError(), 100), ink, 0.75f);
        }

        // Settings own the content area and footer; never draw a second screen over them.
        draw_header.finish();
        performance::SlowScope draw_chat(performance::SlowPhase::Ui,"draw_chat");
        if (!settings_open && !stickers_open && !tg.topicsOpen()) {
        if (tg.currentChatId() != 0) {
            const auto& msgs = tg.messages();
            if (video_fullscreen && message_index >= 0 && message_index < (int)msgs.size()) {
                vita2d_draw_rectangle(0, 0, 960, 544, dark ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255));
                vita2d_texture* picture = animation.texture();
                if (!picture) picture = textures.get(msgs[message_index].media_path);
                if (picture) {
                    float w = vita2d_texture_get_width(picture), h = vita2d_texture_get_height(picture);
                    float scale = std::min(960.0f/w, 544.0f/h);
                    draw_texture_fit(picture, (960-w*scale)/2, (544-h*scale)/2, 960, 544);
                } else draw_text(font, 360, 275, tr("video.loading"), ink, 0.7f);
            } else if (photo_open && message_index >= 0 && message_index < (int)msgs.size() &&
                photo_index < (int)msgs[message_index].photos.size()) {
                const PhotoItem& photo = msgs[message_index].photos[photo_index];
                vita2d_draw_rectangle(0, 36, 960, 508, dark ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255));
                vita2d_texture* image = textures.get(photo.full_path.empty() ? photo.thumbnail_path : photo.full_path);
                if (image) photo_view.draw(image);
                else draw_text(font, 390, 290, tr("photo.loading_full"), ink, 0.75f);
                if (photo.full_path.empty() && image) draw_text(font, 30, 480, tr("photo.loading_larger"), ink, 0.65f);
                char counter[32];
                std::snprintf(counter, sizeof(counter), "%d / %d", photo_index + 1, (int)msgs[message_index].photos.size());
                draw_text(font, 440, 60, counter, ink, 0.65f);
            } else if (post_open && message_index >= 0 && message_index < (int)msgs.size() && msgs[message_index].poll.id) {
                const auto& m = msgs[message_index]; const auto& poll = m.poll;
                draw_post_date(font, m, 0, selected, muted);
                auto question = wrap_text(font, poll.question, 880);
                for (size_t i=0; i<question.size() && i<2; ++i) draw_text(font, 32, 92+i*26, question[i], ink, 0.7f);
                std::string kind = poll.quiz ? tr("poll.kind.quiz") : tr("poll.kind.poll");
                kind += poll.anonymous ? tr("poll.anonymous") : tr("poll.public");
                kind += poll.multiple ? tr("poll.multiple") : tr("poll.single");
                draw_text(font, 32, 148, kind, muted, 0.52f);
                int first = poll_index / 5 * 5;
                for (int row=first; row<(int)poll.order.size() && row<first+5; ++row) {
                    int index = poll.order[row]; const auto& option = poll.options[index];
                    int y = 160 + (row-first)*55;
                    rounded_rect(26, y, 908, 50, row==poll_index ? selected : separator);
                    bool checked = std::find(poll_choices.begin(), poll_choices.end(), index)!=poll_choices.end();
                    std::string label = std::string(checked ? "[x] " : "[ ] ") + option.text;
                    if (option.chosen) label += tr("poll.your_answer");
                    if (option.correct) label += tr("poll.correct");
                    draw_text(font, 38, y+21, fit_text(font, label, 870, 0.57f), ink, 0.57f);
                    if (poll.results || poll.closed || poll.answered()) {
                        vita2d_draw_rectangle(38, y+39, 650.0f*option.percent/100, 4, accent);
                        draw_text(font, 710, y+43, std::to_string(option.percent)+"% • "+std::to_string(option.votes), muted, 0.48f);
                    }
                }
                std::string state = poll.closed ? tr("poll.state.closed") : poll.restricted ? tr("poll.state.restricted") :
                    (!poll.canVote() && poll.answered()) ? tr("poll.state.locked") : tr("poll.state.choose");
                std::string request = tg.pollStatus(m.id);
                if (!request.empty()) state = request;
                draw_text(font, 32, 457, fit_text(font, state+tr("poll.voters", {std::to_string(poll.voters)}), 890, 0.53f), ink, 0.53f);
                if (!poll.explanation.empty()) draw_text(font, 32, 478, fit_text(font, poll.explanation, 890, 0.5f), muted, 0.5f);
            } else if (post_open && message_index >= 0 && message_index < (int)msgs.size() && scrollable_post(msgs[message_index])) {
                const auto& message = msgs[message_index];
                auto lines = post_lines(font, message);
                post_text_scroll = std::max(0, std::min(post_text_scroll, post_scroll_max(message, lines.size())));
                vita2d_enable_clipping();
                vita2d_set_clip_rectangle(0, 56, 960, 544);
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
                if (message.animation || message.sticker) {
                    if (!decode_cooldown) tg.ensureMessageMedia((size_t)message_index);
                    int media_y = post_media_top-post_text_scroll;
                    if (media_y+post_media_height > 38 && media_y < 544) {
                        vita2d_texture* picture = message.animation ? animation.texture() : nullptr;
                        if (!picture && !message.video_note) picture = textures.get(message.media_path);
                        if (picture) {
                            float w = vita2d_texture_get_width(picture), h = vita2d_texture_get_height(picture);
                            float scale = std::min(900.0f/w, (float)post_media_height/h);
                            draw_texture_fit(picture, 30+(900-w*scale)/2, media_y+(post_media_height-h*scale)/2,
                                900, post_media_height);
                        }
                    }
                    std::string state = message.animation_error;
                    if (state.empty() && message.animation_path.empty() && message.animation) {
                        if (message.animation_file_id <= 0) state = tr("media.unavailable");
                        else if (message.animation_size > 0)
                            state = tr("media.loading", {std::to_string(std::min<int64_t>(99,
                                message.animation_downloaded * 100 / message.animation_size)) + "%"});
                        else state = tr("media.loading", {tr("unit.kib", {std::to_string(message.animation_downloaded / 1024)})});
                    } else if (state.empty() && message.animation) state =
                        message.video && !message.video_note && message.video_width > 0 && message.video_height > 0 &&
                        !VideoQuality::hardwareRasterSupported(message.video_width, message.video_height)
                        ? (prepare_video ? animation.status() : tr("video.unsupported_hint")) : animation.status();
                    if (message.video && !message.animation_path.empty()) {
                        int elapsed = animation.seconds(), duration = message.video_duration;
                        char timer[64]; std::snprintf(timer, sizeof(timer), "%d:%02d / %d:%02d  ", elapsed/60, elapsed%60, duration/60, duration%60);
                        state = timer + state;
                        if (duration > 0) vita2d_draw_rectangle(30, media_y+post_media_height+4,
                            900.0f * std::min(elapsed, duration) / duration, 3, accent);
                    }
                    if (!state.empty()) draw_text(font, 30, media_y+post_media_height+23,
                        fit_text(font, state, 900, 0.55f), muted, 0.55f);
                }
                int text_y = post_text_top(message)-post_text_scroll;
                for (int line=0; line<(int)lines.size(); ++line) {
                    int baseline = text_y+line*24;
                    if (baseline < 38) continue;
                    if (baseline-22 >= 544) break;
                    draw_post_line(font, 38, baseline, lines[line], ink, accent, tg, textures, decode_cooldown);
                }
                vita2d_disable_clipping();
                int maximum = post_scroll_max(message, lines.size());
                if (maximum > 0) {
                    float thumb = std::max(28.0f, 488.0f*488/(488+maximum));
                    vita2d_draw_rectangle(953, 56+(488-thumb)*post_text_scroll/maximum, 3, thumb, accent);
                }
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
                int text_y = message.voice ? 330 : (count == 0 ? 110 : (count > 2 ? 470 : 330));
                if (message.voice) {
                    vita2d_draw_rectangle(160, 90, 640, 210, selected);
                    std::string state = message.voice_error;
                    if (state.empty() && message.voice_path.empty()) {
                        if (!message.voice_file_id) state = tr("audio.unavailable");
                        else if (message.voice_size > 0) state = tr("audio.loading", {std::to_string(std::min<int64_t>(99, message.voice_downloaded * 100 / message.voice_size)) + "%"});
                        else state = tr("audio.loading", {tr("unit.kib", {std::to_string(message.voice_downloaded / 1024)})});
                    } else if (state.empty()) state = voice.status();
                    draw_text(font, 184, 130, fit_text(font, message.audio ? message.audio_title : tr("audio.voice_message"), 590, 0.8f), ink, 0.8f);
                    if (message.audio) draw_text(font, 184, 157, fit_text(font, message.audio_performer, 590, 0.6f), muted, 0.6f);
                    draw_text(font, 184, 190, fit_text(font, state, 590, 0.7f), accent, 0.7f);
                    int elapsed = voice.seconds(), duration = message.voice_duration;
                    char time[64]; std::snprintf(time, sizeof(time), "%d:%02d / %d:%02d", elapsed / 60, elapsed % 60, duration / 60, duration % 60);
                    draw_text(font, 184, 237, time, muted, 0.7f);
                    vita2d_draw_rectangle(184, 263, 592, 6, separator);
                    if (duration > 0) vita2d_draw_rectangle(184, 263, 592.0f * std::min(elapsed, duration) / duration, 6, accent);
                }
                if (count > 6) {
                    draw_text(font, 785, 480, tr("photo.more", {std::to_string(count - 6)}), muted, 0.65f);
                }
                auto text_lines = post_lines(font, message);
                int available_lines = std::max(1, (490 - text_y) / 24);
                post_text_scroll = std::max(0, std::min(post_text_scroll,
                    std::max(0, (int)text_lines.size() - available_lines)));
                for (int line = 0; line < available_lines && post_text_scroll + line < (int)text_lines.size(); ++line)
                    draw_post_line(font, 38, text_y + line * 24, text_lines[post_text_scroll + line],
                        message.outgoing ? outgoing : ink, accent, tg, textures, decode_cooldown);
            } else {
                auto history_rows = history_layout(msgs, message_index, tg.currentChatIsGroup());
                vita2d_enable_clipping();
                vita2d_set_clip_rectangle(0, 56, 960, 544);
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
                    if (m.service_message) {
                        std::string label = fit_text(font, tg.serviceMessageText(m), 860, 0.55f);
                        int width = rich_text_width(font, 0.55f, label.c_str()) + 28;
                        int left = 480-width/2;
                        if (i == message_index) rounded_rect(left-2, y-2, width+4, 28, accent);
                        rounded_rect(left, y, width, 24,
                            dark ? RGBA8(35,57,76,255) : RGBA8(185,214,235,255));
                        draw_text(font, left+14, y+17, label, ink, 0.55f);
                        continue;
                    }
                    bool show_sender = tg.currentChatIsGroup() && !m.outgoing;
                    std::string sender = show_sender ? fit_text(font, tg.senderName(m), 654, 0.55f) : "";
                    bool selected_message = i == message_index;
                    std::string preview_path = m.document ? "" : !m.photos.empty() ? m.photos.front().thumbnail_path : m.media_path;
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
                    text_width = std::max(text_width, rich_text_width(font, 0.52f, m.media_label.c_str()) + (m.document ? 17 : 0));
                    int bubble_width = std::min(712, text_width + media_width + 32);
                    if (show_sender) bubble_width = std::max(bubble_width, rich_text_width(font, 0.55f, sender.c_str()) + 58);
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
                        vita2d_texture* sender_avatar = textures.get(tg.senderAvatar(m));
                        if (sender_avatar) draw_texture_fit(sender_avatar, bubble_x+14, y-3, 18, 18);
                        else vita2d_draw_fill_circle(bubble_x+23, y+6, 8, sender_color);
                        draw_text(font, bubble_x+40, y+13, sender, sender_color, 0.55f);
                        y += 20;
                    }
                    // Small tail points toward the sender's side.
                    const int footer_offset = m.media_label.empty() ? 0 : 22;
                    vita2d_draw_rectangle(m.outgoing ? bubble_x+bubble_width-8 : bubble_x, y+54+footer_offset, 8, 14, bubble_color);
                    if (media) draw_texture_fit(media, (float)bubble_x+10, (float)y+4, 96, 56);
                    else if (has_media) draw_text(font, bubble_x+12, y+32, m.video_note ? tr("chat.thumb.round") : m.video ? tr("chat.thumb.video") : m.animation ? "GIF" : "...", muted, 0.68f);
                    int text_x = bubble_x + 16 + media_width;
                    for (size_t row = 0; row < lines.size(); ++row)
                        draw_text(font, text_x, y+20+(int)row*22, lines[row], ink, 0.68f);
                    if (!m.media_label.empty()) {
                        int label_x = text_x;
                        if (m.document) {
                            vita2d_draw_rectangle(label_x, y+48, 10, 12, muted);
                            vita2d_draw_rectangle(label_x+7, y+48, 3, 3, bubble_color);
                            label_x += 15;
                        }
                        draw_text(font, label_x, y + 61, m.media_label, muted, 0.52f);
                    }
                    std::string timestamp = message_time(m);
                    int time_right = bubble_x + bubble_width - (m.outgoing ? 42 : 12);
                    draw_text(font, time_right - rich_text_width(font, 0.52f, timestamp.c_str()), y+61+footer_offset, timestamp, muted, 0.52f);
                    if (m.outgoing) draw_delivery((float)(bubble_x+bubble_width-37), (float)y+50+footer_offset,
                                                  m.delivery, bubble_color, now);
                }
                vita2d_disable_clipping();
                if (msgs.empty()) draw_text(font, 38, 150, tr("chat.no_messages"), muted, 0.75f);
                draw_attach_button(dark);
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
                vita2d_set_clip_rectangle(20, 56, 940, 100);
                for (int i = 0; i < (int)tg.folders().size(); ++i) {
                    int x = 20 + i * 180 - folder_scroll;
                    if (x + 176 <= 20 || x >= 940) continue;
                    vita2d_draw_rectangle(x, 58, 176, 39, i == active ? selected : white);
                    draw_text(font, x+10, 83, fit_text(font, tg.folders()[i].title, 156, 0.65f), i == active ? accent : ink, 0.65f);
                    if (i == active) vita2d_draw_rectangle(x, 95, 176, 3, accent);
                }
                vita2d_disable_clipping();
                if (folder_scroll > 0) draw_text(font, 2, 84, "<", accent, 0.6f);
                if (folder_scroll < folder_max_scroll) draw_text(font, 944, 84, ">", accent, 0.6f);
            } else { shown_folder_id = -1; shown_folder_index = -1; }
            chat_scroll.configure((int)chats.size(), chat_row_height, 544-chat_top);
            const int base = chat_scroll.first(), end = chat_scroll.end();
            int y = chat_top + 24 + base * chat_row_height - chat_scroll.offset();
            if (!decode_cooldown) for (int i = base; i < end; ++i) tg.ensureChatMedia((size_t)i);
            vita2d_enable_clipping();
            vita2d_set_clip_rectangle(20, chat_top, 940, 544);
            for (int i = base; i < end; ++i) {
                vita2d_draw_rectangle(20, y - 24, 920, chat_row_height, i == chat_index ? selected : white);
                vita2d_draw_rectangle(88, y - 25 + chat_row_height, 852, 1, separator);
                vita2d_texture* avatar = textures.get(chats[i].avatar_path);
                if (avatar) draw_texture_fit(avatar, 34, (float)y - 21, 42, 42);
                else vita2d_draw_rectangle(34, y - 21, 42, 42, RGBA8(126,176,211,255));
                draw_text(font, 88, y + 3, clip(chats[i].title, 70), ink, 0.72f);
                y += chat_row_height;
            }
            vita2d_disable_clipping();
            if (chats.empty()) draw_text(font, 38, chat_top+51, tg.chatsLoading() ? tr("chats.loading_first") :
                (tg.chatsEnd() ? tr("chats.none") : tr("chats.load_failed")), muted, 0.75f);
            vita2d_draw_rectangle(0, 36, 960, 18, selected);
            const std::string progress = tr("chats.status", {tg.chatsLoading() ? tr("chats.state_loading") : tr("chats.state_ready"),
                std::to_string(chats.size()), tg.chatsEnd() ? tr("chats.all_loaded") : tr("chats.tap_more")});
            const Updater::Snapshot update_state = updater.snapshot();
            const std::string update_hint = update_state.state == Updater::State::Available ? tr("update.hint", {update_state.version}) + "   " : "";
            draw_text(font, 28, 49, !update_notice.empty() ? fit_text(font, update_notice, 900, 0.48f) :
                fit_text(font, update_hint + (tg.chatsError().empty() ? progress : tr("chats.retry", {tg.chatsError()})), 900, 0.48f), ink, 0.48f);
            if (tg.chatsLoading()) {
                float position = (float)((now / 2000) % 840);
                vita2d_draw_rectangle(position, 52, 120, 2, accent);
            }
        } else if (!tg.qrLink().empty()) {
            static std::string qr_text;
            static QrCode qr;
            if (qr_text != tg.qrLink()) { qr_text = tg.qrLink(); if (!qr_encode(qr_text, qr)) qr = QrCode(); }
            draw_text(font, 30, 70, tr("auth.qr_title"), ink, 0.8f);
            if (qr.size) {
                const int module = std::max(2, 400 / (qr.size + 8));
                const int side = (qr.size + 8) * module, left = 30, top = 90;
                vita2d_draw_rectangle(left, top, side, side, RGBA8(255,255,255,255));
                for (int y = 0; y < qr.size; ++y)
                    for (int x = 0; x < qr.size; ++x)
                        if (qr.dark(x, y)) vita2d_draw_rectangle(left + (x + 4) * module, top + (y + 4) * module, module, module, RGBA8(0,0,0,255));
            }
            int line_y = 120;
            for (const auto& line : wrap_text(font, tr("auth.qr_steps"), 440)) { draw_text(font, 490, line_y, line, ink, 0.62f); line_y += 28; }
            draw_text(font, 28, 528, tr("auth.qr_footer"), ink, 0.6f);
        } else {
            bool starting = tg.inputKind() == InputKind::None;
            draw_text(font, 30, 145, starting ? tr("auth.connecting") : tr("auth.title"), ink, 0.9f);
            draw_text(font, 30, 180, clip(tg.status(), 100), muted, 0.7f);
            if (!tg.authDetail().empty() && tg.inputKind() == InputKind::Code)
                draw_text(font, 30, 222, fit_text(font, tg.authDetail(), 900, 0.62f), ink, 0.62f);
            if (!tg.authResendInfo().empty() && tg.inputKind() == InputKind::Code)
                draw_text(font, 30, 252, fit_text(font, tg.authResendInfo(), 900, 0.58f), muted, 0.58f);
            if (tg.canGoBackInput())
                draw_text(font, 30, 490, tg.canResendCode() ? tr("auth.code_hint") : tr("auth.back_hint"), muted, 0.6f);
            if (tg.canRequestQr()) draw_text(font, 30, 462, tr("auth.qr_hint"), muted, 0.6f);
            vita2d_draw_rectangle(30, 280, 440, 60, selected);
            draw_text(font, 48, 318, tr("auth.setup_proxy"), ink, 0.7f);
            draw_text(font, 30, 375, tr("auth.proxy_hint"), muted, 0.6f);
            draw_text(font, 30, 528, starting ? tr("auth.waiting_connection") : tr("auth.continue"), ink, 0.6f);
            draw_text(font, 510, 528, tr("hint.mtproxy"), ink, 0.6f);
            if (starting && net_ok && tg.lastError().empty()) {
                int phase = (int)((now / 100000) % 8);
                for (int dot = 0; dot < 8; ++dot)
                    vita2d_draw_rectangle(32 + dot * 24, 205, 14, 14, dot == phase ? accent : separator);
                draw_text(font, 30, 490, tr("auth.waiting_status"), muted, 0.65f);
            }
        }

        } // !settings_open
        draw_chat.finish();

        performance::SlowScope draw_overlays(performance::SlowPhase::Ui,"draw_overlays");
        if (!settings_open && !stickers_open && !tg.topicsOpen() && !photo_open && !video_fullscreen && tg.currentChatId()) {
            // Status strip sits right under the header so it never covers media.
            vita2d_draw_rectangle(0, 36, 960, 18, selected);
            int progress = tg.historyPercent();
            vita2d_draw_rectangle(0, 52, 960.0f * progress / 100, 2, accent);
            std::string thread_info = settings.show_threads ? " | " + cpu_scheduler::threadStatus() : "";
            const std::string loading = tr(tg.historyLoading() ? "history.block_loading" : "history.block_ready",
                {std::to_string(progress)}) + thread_info;
            std::string audio_status;
            if (recorder.processing() && audio_context) {
                audio_status = tr("audio.status", {recorder.status()});
            } else if (recorder.recording() && audio_context) {
                audio_status = recorder.peak() ? tr("audio.mic_active") : tr("audio.no_signal");
            } else if (!tg.uploadStatus().empty()) {
                audio_status = tg.uploadStatus();
            } else if (!recorder_status.empty() && recorder_status != tr("rec.recording")) {
                audio_status = tr("audio.status", {recorder_status});
            }
            std::string info = !tg.documentStatus().empty() ? tg.documentStatus() : !browser_status.empty() ? browser_status :
                (!audio_status.empty() ? audio_status :
                (tg.historyError().empty() ? loading : tr("history.error", {tg.historyError()})));
            draw_text(font, 28, 49, fit_text(font, info, recorder.recording() ? 650 : 904, 0.48f), ink, 0.48f);
            if (recorder.recording()) {
                vita2d_draw_rectangle(690, 39, 235, 8, separator);
                vita2d_draw_rectangle(690, 39, 2.35f * recorder.peak(), 8, accent);
                vita2d_draw_rectangle(690, 48, 235, 3, muted);
                vita2d_draw_rectangle(690, 48, 2.35f * recorder.level(), 3, accent);
            }
        }
        if (!settings_open && !tg.currentChatId() && !tg.uploadStatus().empty()) {
            vita2d_draw_rectangle(0, 36, 960, 18, selected);
            draw_text(font, 28, 49, fit_text(font, tg.uploadStatus(), 900, 0.48f), ink, 0.48f);
        }
        if (!settings_open && !tg.ready() && tg.canBrowse() && !kb.active()) {
            vita2d_draw_rectangle(0, 78, 960, 26, RGBA8(20,35,50,240));
            draw_text(font, 20, 98, tr("diag.cached_waiting"), header_text, 0.55f);
        }
        if (!settings_open && reactions_open) {
            vita2d_draw_rectangle(0, 36, 960, 508, white);
            draw_text(font, 32, 80, tr("reaction.title"), ink, 0.85f);
            draw_text(font, 32, 112, fit_text(font, tg.reactionStatus(), 880, 0.6f), muted, 0.6f);
            const auto& choices = tg.reactionChoices();
            int begin = reaction_index/32*32;
            for (int i=begin; i<(int)choices.size() && i<begin+32; ++i) {
                int x=32+(i-begin)%8*112, y=130+(i-begin)/8*72;
                rounded_rect(x, y, 104, 64, i == reaction_index ? selected : separator);
                if (!decode_cooldown) tg.ensureReactionMedia(i);
                auto icon=textures.get(tg.reactionIcon(i));
                if (icon) draw_texture_fit(icon,x+28,y+8,48,48);
                else draw_text(font, x+8, y+43, fit_text(font,choices[i],88,0.7f), ink, 0.7f);
            }
        }
        if (!settings_open && tg.topicsOpen()) {
            vita2d_draw_rectangle(0, 36, 960, 508, white);
            draw_text(font, 30, 61, tr("topics.title"), ink, 0.7f);
            const auto& topics = tg.topics();
            int first = std::max(0, topic_index-7);
            for (int i = first; i < std::min(first+8, (int)topics.size()); ++i) {
                int y = 70+(i-first)*50;
                vita2d_draw_rectangle(20, y, 920, 48, i == topic_index ? selected : white);
                std::string title = topics[i].title + (topics[i].closed ? tr("topics.closed") : "");
                draw_text(font, 34, y+32, fit_text(font, title, 740, 0.75f), ink, 0.75f);
                if (topics[i].unread) draw_text(font, 830, y+32, std::to_string(topics[i].unread), accent, 0.7f);
            }
            draw_text(font, 30, 492, fit_text(font, tg.topicsStatus(), 900, 0.55f), muted, 0.55f);
        }
        if (!settings_open && stickers_open) {
            vita2d_draw_rectangle(0, 36, 960, 508, white);
            const auto& packs = tg.stickerPacks();
            if (sticker_pack_menu) {
                draw_text(font, 30, 76, tr("sticker.sets_title"), ink, 0.8f);
                draw_text(font, 30, 108, fit_text(font, tg.stickerStatus(), 900, 0.55f), muted, 0.55f);
                int first = std::max(0, sticker_pack - 6);
                for (int i = first; i < std::min(first + 7, (int)packs.size()); ++i) {
                    int y = 116 + (i-first)*54;
                    vita2d_draw_rectangle(24, y, 912, 48, i == sticker_pack ? selected : white);
                    if (!decode_cooldown) tg.ensureStickerPackIcon(i);
                    auto icon = textures.get(packs[i].icon_path);
                    if (icon) draw_texture_fit(icon, 30, y+4, 40, 40);
                    else { vita2d_draw_rectangle(30, y+6, 36, 36, separator); draw_text(font, 40, y+32, "S", ink, 0.6f); }
                    draw_text(font, 82, y+31, fit_text(font, packs[i].title, 820, 0.65f), ink, 0.65f);
                }
            } else {
                std::string title = sticker_pack < (int)packs.size() ? packs[sticker_pack].title : tr("sticker.title");
                draw_text(font, 30, 76, fit_text(font, title, 900, 0.8f), ink, 0.8f);
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
                draw_text(font, 30, 490, tr("sticker.page", {std::to_string(first/18+1), std::to_string(std::max(1, ((int)items.size()+17)/18))}), muted, 0.6f);
            }
        }
        draw_overlays.finish();
        if (circle_recorder.active()) {
            using RecordingState = VideoNoteRecorder::State;
            auto state = circle_recorder.state();
            vita2d_draw_rectangle(0, 0, 960, 544, white);
            draw_text(font, 28, 34, tr("vnr.title"), ink, 0.85f);
            draw_text(font, 590, 34, circle_recorder.rearCamera() ? tr("vnr.rear_camera") : tr("vnr.front_camera"), muted, 0.6f);
            auto* preview = circle_playback && circle_playback->texture() ? circle_playback->texture() : circle_recorder.preview();
            if (preview) draw_texture_fit(preview, 290, 56, 380, 380);
            std::string timer = tr("vnr.timer", {std::to_string(circle_recorder.seconds())});
            if (state == RecordingState::Encoding) timer = tr("vnr.encoding_percent", {std::to_string(circle_recorder.percent())});
            draw_text(font, 350, 470, timer, state == RecordingState::Recording ? RGBA8(210,45,45,255) : ink, 0.7f);
            if (state == RecordingState::Recording) {
                vita2d_draw_rectangle(320, 490, 320, 6, separator);
                vita2d_draw_rectangle(320, 490, 3.2f * circle_recorder.level(), 6, accent);
            }
            draw_text(font, 28, 528, fit_text(font, circle_error.empty() ? circle_recorder.status() : circle_error, 700, 0.55f), ink, 0.55f);
        }
        if (chat_ui_visible()) {
            float t = chat_menu_open ? std::min(1.f, (float)(now - chat_menu_since) / 140000.f) : 0.f;
            const float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
            if (chat_menu_open) {
                const auto items = chat_menu_items();
                const float h = (float)items.size() * kMenuRowHeight;
                const float dx = (1.f - e) * 36.f, dy = (1.f - e) * 30.f, y0 = kMenuBottom - h + dy;
                glass_panel(kMenuX + dx, y0, kMenuWidth, h, scale_alpha(RGBA8(52, 58, 68, 232), e), scale_alpha(RGBA8(20, 25, 33, 238), e));
                vita2d_draw_rectangle(kMenuX + 10 + dx, y0, kMenuWidth - 20, 1, scale_alpha(RGBA8(255, 255, 255, 90), e));
                chat_menu_sel = std::max(0, std::min(chat_menu_sel, (int)items.size() - 1));
                for (int i = 0; i < (int)items.size(); ++i) {
                    if (i == chat_menu_sel) glass_panel(kMenuX + 4 + dx, y0 + i * kMenuRowHeight + 2, kMenuWidth - 8, kMenuRowHeight - 4,
                        scale_alpha(RGBA8(255, 255, 255, 62), e), scale_alpha(RGBA8(255, 255, 255, 26), e), 7);
                    if (i) vita2d_draw_rectangle(kMenuX + 8 + dx, y0 + i * kMenuRowHeight, kMenuWidth - 16, 1, scale_alpha(RGBA8(255, 255, 255, 36), e));
                    draw_text(font, kMenuX + 18 + dx, y0 + i * kMenuRowHeight + 26, fit_text(font, tr(items[i].label), kMenuWidth - 36, 0.62f),
                              scale_alpha(RGBA8(240, 244, 249, 255), e), 0.62f);
                }
            }
            draw_menu_button(dark, chat_menu_open ? e : 0.f);
        }
        {
            static UploadPicker::Stage previous_stage = UploadPicker::Stage::Closed;
            if (upload_picker.stage == UploadPicker::Stage::Menu && previous_stage != UploadPicker::Stage::Menu) attach_menu_since = now;
            previous_stage = upload_picker.stage;
        }
        if (upload_picker.active()) {
            using Stage = UploadPicker::Stage;
            const char* kind_key[] = {"upload.photo", "upload.video", "upload.file"};
            if (upload_picker.stage == Stage::Menu) {
                // Compact glass popup growing out of the attach button (~140 ms ease-out).
                float t = std::min(1.f, (float)(now - attach_menu_since) / 140000.f);
                const float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
                const float dy = (1.f - e) * 30.f, dx = -(1.f - e) * 36.f; // grows out of the circle
                glass_panel(40 + dx, 366 + dy, 230, 120, scale_alpha(RGBA8(52, 58, 68, 232), e), scale_alpha(RGBA8(20, 25, 33, 238), e));
                vita2d_draw_rectangle(50 + dx, 366 + dy, 210, 1, scale_alpha(RGBA8(255, 255, 255, 90), e));
                for (int i = 0; i < 3; ++i) {
                    if (i == upload_picker.selected) glass_panel(44 + dx, 370 + i * 40 + dy, 222, 36, scale_alpha(RGBA8(255, 255, 255, 62), e), scale_alpha(RGBA8(255, 255, 255, 26), e), 7);
                    if (i) vita2d_draw_rectangle(48 + dx, 366 + i * 40 + dy, 214, 1, scale_alpha(RGBA8(255, 255, 255, 36), e));
                    draw_text(font, 60 + dx, 396 + i * 40 + dy, tr(kind_key[i]), scale_alpha(RGBA8(240, 244, 249, 255), e), 0.7f);
                }
                draw_attach_button(dark, e);
            } else if (upload_picker.stage == Stage::Browse) {
                vita2d_draw_rectangle(0, 36, 960, 508, white);
                const bool preview = upload_picker.kind == UploadPicker::Kind::Photo;
                const float list_w = preview ? 600.f : 896.f;
                draw_text(font, 30, 68, fit_text(font, upload_picker.path.empty() ? tr("picker.drives") : upload_picker.path == UploadPicker::LatestView ? tr("upload.latest") : upload_picker.path, 900, 0.64f), ink, 0.64f);
                draw_text(font, 32, 97, fit_text(font, tr(kind_key[(int)upload_picker.kind]) + "  |  " +
                    tr(upload_picker.show_hidden ? "upload.hidden_on" : "upload.hidden_off"), 900, 0.48f), muted, 0.48f);
                rounded_rect(24, 109, preview ? 616 : 912, 356, separator);
                rounded_rect(26, 111, preview ? 612 : 908, 352, white);
                const int first = (upload_picker.selected / 8) * 8;
                for (int row = 0; row < 8 && first + row < (int)upload_picker.entries.size(); ++row) {
                    const auto& entry = upload_picker.entries[first + row];
                    if (first + row == upload_picker.selected) rounded_rect(32, 117 + row * 43, list_w, 40, selected);
                    draw_text(font, 42, 144 + row * 43, fit_text(font, !entry.label.empty() ? entry.label : entry.dir ? entry.name + "/" : entry.name, list_w - (entry.date.empty() ? 30 : 220), 0.65f), ink, 0.65f);
                    if (!entry.date.empty()) draw_text(font, 32 + list_w - 190, 143 + row * 43, entry.date, muted, 0.45f);
                    if (!entry.dir) draw_text(font, 32 + list_w - 72, 143 + row * 43, UploadPicker::formatSize(entry.size), muted, 0.45f);
                }
                if (preview) {
                    rounded_rect(646, 109, 290, 356, separator);
                    rounded_rect(648, 111, 286, 352, white);
                    if (upload_picker.selected >= 0 && upload_picker.selected < (int)upload_picker.entries.size() &&
                        !upload_picker.entries[upload_picker.selected].dir) {
                        const std::string full = upload_picker.fullPath(upload_picker.entries[upload_picker.selected]);
                        draw_texture_fit(textures.get(full), 652, 117, 278, 340);
                    }
                }
                if (upload_picker.entries.empty() && upload_picker.error.empty())
                    draw_text(font, 42, 144, fit_text(font, upload_picker.info.empty() ? tr("upload.empty") : upload_picker.info, 520, 0.5f), muted, 0.5f);
                if (!upload_picker.error.empty()) draw_text(font, 30, 484, upload_picker.error, ink, 0.55f);
                draw_text(font, 28, 528, tr("upload.browse_footer"), ink, 0.6f);
            } else {
                vita2d_draw_rectangle(0, 36, 960, 508, white);
                const int count = (int)upload_picker.items.size();
                draw_text(font, 30, 76, tr("upload.review_title", {std::to_string(count), std::to_string(UploadPicker::MaxItems)}), ink, 0.8f);
                rounded_rect(24, 109, 512, 356, separator);
                rounded_rect(26, 111, 508, 352, white);
                const int first = (upload_picker.selected / 8) * 8;
                const char* add_key[] = {"upload.add_photo", "upload.add_video", "upload.add_file"};
                for (int row = 0; row < 8 && first + row < upload_picker.reviewRows(); ++row) {
                    const int index = first + row;
                    if (index == upload_picker.selected) rounded_rect(32, 117 + row * 43, 496, 40, selected);
                    std::string label;
                    if (index < count) label = upload_picker.items[index].name;
                    else if (index < count + 3) label = tr(add_key[index - count]);
                    else label = tr("upload.send", {std::to_string(count)});
                    draw_text(font, 42, 144 + row * 43, fit_text(font, label, index < count ? 400 : 470, 0.65f),
                              index == count + 3 && count == 0 ? muted : ink, 0.65f);
                    if (index < count) draw_text(font, 450, 143 + row * 43, UploadPicker::formatSize(upload_picker.items[index].size), muted, 0.45f);
                }
                rounded_rect(548, 109, 388, 356, separator);
                rounded_rect(550, 111, 384, 352, white);
                const auto* focus = upload_picker.focusedItem();
                if (focus) {
                    if (focus->kind == UploadPicker::Kind::Photo) draw_texture_fit(textures.get(focus->path), 556, 117, 372, 340);
                    else if (focus->kind == UploadPicker::Kind::Video) {
                        draw_texture_fit(animation.texture(), 556, 117, 372, 310);
                        draw_text(font, 560, 452, fit_text(font, animation.status(), 360, 0.45f), muted, 0.45f);
                    } else draw_text(font, 566, 150, fit_text(font, tr("upload.file_info", {focus->name, UploadPicker::formatSize(focus->size)}), 350, 0.55f), muted, 0.55f);
                }
                draw_text(font, 28, 528, tr(focus && focus->kind == UploadPicker::Kind::Video ? "upload.review_footer_video" : "upload.review_footer"), ink, 0.55f);
            }
        }
        if (kb.active()) kb.draw(font);
        if (unsupported_video_prompt) {
            vita2d_draw_rectangle(95, 145, 770, 255, white);
            vita2d_draw_rectangle(95, 145, 770, 4, accent);
            draw_text(font, 125, 195, tr("unsupported.title"), ink, 0.8f);
            draw_text(font, 125, 242, tr("unsupported.reason"), ink, 0.6f);
            draw_text(font, 125, 286, tr("unsupported.question"), ink, 0.6f);
            draw_text(font, 125, 318, tr("unsupported.warning"), ink, 0.6f);
            draw_text(font, 125, 368, tr("unsupported.footer"), muted, 0.6f);
        }

        performance::SlowScope draw_end(performance::SlowPhase::Ui,"draw_end");
        vita2d_end_drawing();
        draw_end.finish();
        draw_stage.finish();
        slow_ui.finish();
        ui_measurement.finish();
        if (input_sampled) performance::observe(performance::Metric::InputSubmit, performance::now()-input_sampled);
        // vita2d reuses one vertex pool on the next frame. Finish this scene
        // before the IME renders into the same surfaces or that pool is reset.
        { performance::Scope measurement(performance::Metric::GpuWait); vita2d_wait_rendering_done(); }
        if (kb.active()) vita2d_common_dialog_update();
        { performance::Scope measurement(performance::Metric::Swap); vita2d_swap_buffers(); }
        if (!settings_open && !stickers_open && !reactions_open && !tg.topicsOpen() && !circle_recorder.active() && !photo_open && !kb.active())
            tg.messagesPresented();
        // CommonDialog submits through its own context; waiting only for the
        // vita2d context doesn't cover the keyboard's GPU work.
        if (kb.active()) { performance::Scope measurement(performance::Metric::GpuWait); sceGxmDisplayQueueFinish(); }
        textures.loadOne();
        rich_text_poll();
        if (!interaction && !decode_cooldown) emoji_load_one();
    }

    vita2d_wait_rendering_done();
    photo_view.updateMotion(false, 0, false);
    qr_scanner.stop();
    circle_playback.reset();
    circle_recorder.cancel();
    animation.shutdown();
    voice.shutdown();
    textures.stop();
    textures.clear();
    rich_text_shutdown();
    vita2d_free_pgf(font);
    vita2d_fini();
    tg.stopWorker();
    net.shutdown();
    sceAppUtilShutdown();
    if (restart_requested) sceAppMgrLoadExec("app0:eboot.bin", nullptr, nullptr);
    sceKernelExitProcess(0);
    return 0;
}
