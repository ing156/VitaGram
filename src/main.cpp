#include "performance_log.h"
#include "startup_trace.h"
#include "webapp_diagnostics.h"
#include "webapp_invoice.h"
#include "i18n.h"
#include "exit_gesture.h"
#include "telegram_client.h"
#if VITAGRAM_WEBKIT
#include "webapp_view.h"
#include "webkit_download.h"
#include "webapp_theme.h"
#endif
#include "history_selection.h"
#include "history_scroll.h"
#include "chat_calendar.h"
#include "chat_bubble.h"
#include "reaction_input.h"
#include "quote_layout.h"
#include "picker_input.h"
#include "radial_menu_view.h"
#include "chat_list_scroll.h"
#include "chat_list_motion.h"
#include "scroll_repeat.h"
#include "keyboard.h"
#include "keyboard_layout.h"
#include "keyboard_languages.h"
#include "vita_net.h"
#include "media_render.h"
#include "ui_skin.h"
#include "ui_layout.h"
#include "chat_view_layout.h"
#include "folder_drawer.h"
#include "theme_picker.h"
#include "media_load_view.h"
#include "media_seek.h"
#include "audio_bar.h"
#include "text_layout_cache.h"
#include "text_font.h"
#include "image_decoder.h"
#include "image_retry.h"
#include "gif_diagnostics.h"
#include <cstring>
#include "animation_player.h"
#include "voice_player.h"
#include "audio_recorder.h"
#include "audio_recording_dialog.h"
#include "spoiler_visibility.h"
#include "transient_status.h"
#include "video_note_recorder.h"
#include "video_quality.h"
#include "config.h"
#include <memory>
#include <unordered_map>
#include "cpu_scheduler.h"
#include "settings.h"
#include "settings_menu.h"
#include "file_download.h"
#include "vpk_installer.h"
#include "vpk_package.h"
#include "download_folder_picker.h"
#include "upload_picker.h"
#include "proxy_link.h"
#include "secure_store.h"
#include "vault.h"
#include "crash_dump.h"
#include "memory_hygiene.h"
#include "process_modules.h"
#include "account_cleanup.h"
#include "qr_encode.h"
#include "qr_render.h"
#include "vault_ui.h"
#include "ui_diagnostics.h"
#include "qr_scanner.h"
#include "photo_view.h"
#include "updater.h"
#include "ui_battery.h"
#include "ui_skin.h"
#include <psp2/rtc.h>
#include <psp2/power.h>
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
#include <map>
#include <set>
#include <vector>

struct CachedTexture {
    std::string path;
    vita2d_texture* texture = nullptr;
    unsigned int used = 0;
    size_t bytes = 0;
    int mask = 0;
};

// Applies the avatar shape to decoded pixels: 0 rounded square, 1 circle. Antialiased through a
// signed-distance edge; alpha only, so it costs nothing at draw time.
static void mask_avatar(DecodedImage& image, int shape) {
    const float w = (float)image.width, h = (float)image.height;
    const float radius = shape == 1 ? std::min(w, h) / 2 : std::min(w, h) * 0.22f;
    for (unsigned y = 0; y < image.height; ++y) {
        for (unsigned x = 0; x < image.width; ++x) {
            const float qx = std::fabs(x + 0.5f - w / 2) - (w / 2 - radius), qy = std::fabs(y + 0.5f - h / 2) - (h / 2 - radius);
            const float distance = std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - radius;
            const float coverage = std::max(0.f, std::min(1.f, 0.5f - distance));
            if (coverage >= 1.f) continue;
            uint8_t& alpha = image.rgba[((size_t)y * image.width + x) * 4 + 3];
            alpha = (uint8_t)(alpha * coverage);
        }
    }
}

// Center square crop, used before the circular mask so round-video thumbnails keep their proportions.
static void crop_center_square(DecodedImage& image) {
    if (image.width == image.height) return;
    const unsigned side = std::min(image.width, image.height);
    const unsigned left = (image.width - side) / 2, top = (image.height - side) / 2;
    std::vector<uint8_t> cropped((size_t)side * side * 4);
    for (unsigned y = 0; y < side; ++y)
        std::memcpy(&cropped[(size_t)y * side * 4], &image.rgba[((size_t)(y + top) * image.width + left) * 4], (size_t)side * 4);
    image.rgba.swap(cropped);
    image.width = image.height = side;
}

// Preview shown for photos behind a spoiler: a heavily softened, darkened and speckled copy. The tiny result is
// stretched by the GPU's linear filter, which smears it further; nothing sharp survives.
static void blur_spoiler(DecodedImage& image) {
    if (!image.width || !image.height) return;
    const unsigned tw = std::max(8u, std::min(48u, image.width));
    const unsigned th = std::max(6u, (unsigned)((unsigned long long)image.height * tw / image.width));
    std::vector<uint8_t> small((size_t)tw * th * 4);
    for (unsigned y = 0; y < th; ++y) {
        for (unsigned x = 0; x < tw; ++x) {
            const unsigned x0 = x * image.width / tw, x1 = std::max(x0 + 1, (x + 1) * image.width / tw);
            const unsigned y0 = y * image.height / th, y1 = std::max(y0 + 1, (y + 1) * image.height / th);
            const unsigned step_x = std::max(1u, (x1 - x0) / 6), step_y = std::max(1u, (y1 - y0) / 6);
            unsigned sum[3] = {0, 0, 0}, count = 0;
            for (unsigned sy = y0; sy < y1 && sy < image.height; sy += step_y)
                for (unsigned sx = x0; sx < x1 && sx < image.width; sx += step_x) {
                    const uint8_t* px = &image.rgba[((size_t)sy * image.width + sx) * 4];
                    sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2]; ++count;
                }
            count = std::max(1u, count);
            uint8_t* out = &small[((size_t)y * tw + x) * 4];
            out[0] = (uint8_t)(sum[0] / count); out[1] = (uint8_t)(sum[1] / count); out[2] = (uint8_t)(sum[2] / count); out[3] = 255;
        }
    }
    std::vector<uint8_t> scratch(small.size());
    for (int pass = 0; pass < 2; ++pass) {   // 3x3 box blur, edges clamped
        for (unsigned y = 0; y < th; ++y)
            for (unsigned x = 0; x < tw; ++x)
                for (int c = 0; c < 3; ++c) {
                    int total = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const unsigned sx = (unsigned)std::max(0, std::min((int)tw - 1, (int)x + dx));
                            const unsigned sy = (unsigned)std::max(0, std::min((int)th - 1, (int)y + dy));
                            total += small[((size_t)sy * tw + sx) * 4 + c];
                        }
                    scratch[((size_t)y * tw + x) * 4 + c] = (uint8_t)(total / 9);
                }
        for (size_t i = 3; i < small.size(); i += 4) scratch[i] = 255;
        small.swap(scratch);
    }
    unsigned seed = 2463534242u;
    for (size_t i = 0; i < (size_t)tw * th; ++i) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        const bool speck = (seed & 31) == 0;
        for (int c = 0; c < 3; ++c) {
            int value = small[i * 4 + c] * 85 / 100;
            (void)speck;
            small[i * 4 + c] = (uint8_t)std::min(255, value);
        }
    }
    image.rgba.swap(small);
    image.width = tw; image.height = th;
}

enum TextureMask { kMaskNone = 0, kMaskAvatar = 1, kMaskRoundThumb = 2, kMaskBlur = 3 };

class TextureCache {
public:
    void beginFrame(bool pause) { paused_ = pause; requests_.clear(); }
    // Avatars are masked to the chosen shape while they are uploaded.
    vita2d_texture* get(const std::string& path, int mask = kMaskNone) {
        if (path.empty()) return nullptr;
        ++clock_;
        for (auto& item : items_) {
            if (item.path == path && (mask != kMaskBlur || item.mask == kMaskBlur)) {
                item.used = clock_;
                return item.texture;
            }
        }
        if (!retries_.ready(path, sceKernelGetSystemTimeWide())) return nullptr;
        if (std::find(requests_.begin(), requests_.end(), path) == requests_.end()) requests_.push_back(path);
        if (mask != kMaskNone) mask_paths_[path] = mask;
        return nullptr;
    }
    // A texture that is already decoded; never queues a load.
    vita2d_texture* find(const std::string& path, int mask = kMaskNone) {
        if (path.empty()) return nullptr;
        for (auto& item : items_)
            if (item.path == path && (mask != kMaskBlur || item.mask == kMaskBlur)) { item.used = ++clock_; return item.texture; }
        return nullptr;
    }
    vita2d_texture* getGif(const std::string& path, const SendGifItem& item, unsigned generation, int index, bool mini = false) {
        if (path.empty()) return nullptr;
        auto found = previews_.find(path);
        if (found == previews_.end() || found->second.generation != generation) {
            if (found == previews_.end() && previews_.size() >= 128) previews_.erase(previews_.begin());
            previews_[path] = {generation, index, item.preview, mini ? "mini" : item.preview_format, false};
        }
        auto* texture = get(path);
        auto& context = previews_[path];
        if (texture && !context.shown) {
            context.shown = true;
            previewLog(path, mini ? "fallback_shown" : "preview_shown", "ok",
                vita2d_texture_get_width(texture), vita2d_texture_get_height(texture));
        }
        return texture;
    }
    void loadOne() {
        decoder_.request(requests_, paused_);
        std::string path;
        DecodedImage image;
        if (!decoder_.take(path, image)) return;
        if (image.width && image.height && image.rgba.size() == size_t(image.width)*image.height*4) {
            const auto mask = mask_paths_.find(path);
            if (mask != mask_paths_.end() && mask->second == kMaskAvatar) mask_avatar(image, avatar_shape_);
            else if (mask != mask_paths_.end() && mask->second == kMaskRoundThumb) { crop_center_square(image); mask_avatar(image, 1); }
            else if (mask != mask_paths_.end() && mask->second == kMaskBlur) blur_spoiler(image);
        }
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
        if (!texture) {
            const auto attempt = retries_.fail(path, sceKernelGetSystemTimeWide());
            previewLog(path, "preview_failed", image.error ? image.error : image.rgba.empty() ? "decode_empty" : "gpu_alloc",
                image.width, image.height, attempt);
            return; // Do not cache nullptr permanently.
        }
        retries_.forget(path);
        previewLog(path, "preview_uploaded", "ok", image.width, image.height);
        // 32 MiB of real CDRAM, leaving the rest to video surfaces and fonts.
        while (!items_.empty() && (used_bytes_ + bytes > 32 * 1024 * 1024 || items_.size() >= 256)) {
            auto oldest = std::min_element(items_.begin(), items_.end(), [](const CachedTexture& a, const CachedTexture& b) {
                return a.used < b.used;
            });
            if (oldest->texture) vita2d_free_texture(oldest->texture);
            used_bytes_ -= oldest->bytes;
            items_.erase(oldest);
        }
        for (auto it = items_.begin(); it != items_.end();) {
            if (it->path != path) { ++it; continue; }
            vita2d_free_texture(it->texture); used_bytes_ -= it->bytes; it = items_.erase(it);
        }
        const auto applied_mask = mask_paths_.find(path);
        items_.push_back({path, texture, clock_, bytes, applied_mask == mask_paths_.end() ? kMaskNone : applied_mask->second});
        used_bytes_ += bytes;
        performance::cdram_images = (int64_t)used_bytes_;
    }
    void stop() { decoder_.stop(); }
    // Drops one cached texture (and its mask) so the next get() decodes it again, e.g. sharp after a spoiler is revealed.
    void forget(const std::string& path) {
        retries_.forget(path);
        previews_.erase(path);
        mask_paths_.erase(path);
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->path != path) continue;
            if (it->texture) { vita2d_wait_rendering_done(); vita2d_free_texture(it->texture); }
            used_bytes_ -= it->bytes;
            items_.erase(it);
            break;
        }
    }
    int avatarShape() const { return avatar_shape_; }
    // Cached avatars carry the old mask: drop everything (caller waits for the GPU first) so they reload.
    void setAvatarShape(int shape) { if (shape != avatar_shape_) { avatar_shape_ = shape; clear(); } }
    void clear() {
        decoder_.clear();
        retries_.clear(); previews_.clear();
        requests_.clear();
        mask_paths_.clear();
        for (auto& item : items_) if (item.texture) vita2d_free_texture(item.texture);
        items_.clear();
        used_bytes_ = 0;
        performance::cdram_images = 0;
    }
private:
    struct PreviewContext { unsigned generation; int item, file; std::string format; bool shown; };
    std::map<std::string, PreviewContext> previews_;
    ImageRetry retries_;
    void previewLog(const std::string& path, const char* stage, const char* reason, int width, int height, int64_t value = 0) {
        auto it = previews_.find(path);
        if (it == previews_.end()) return;
        const auto& c = it->second;
        gif_diagnostics::preview(stage, c.format.c_str(), c.generation, c.item + 1, c.file,
            width, height, 0, reason, performance::now(), value);
    }
    ImageDecoder decoder_;
    bool paused_ = false;
    size_t used_bytes_ = 0;
    int avatar_shape_ = 0;
    std::map<std::string, int> mask_paths_;
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

static void draw_spoiler_particles(float x, float y, float w, float h, int64_t now, int64_t seed) {
    if (w < 6 || h < 6) return;
    const int count = std::max(12, std::min(180, int(w*h/750)));
    const float time = float((now % 60000000) / 1000000.0);
    uint32_t state = uint32_t(seed) ^ uint32_t(uint64_t(seed) >> 32) ^ 0x9e3779b9u;
    auto random = [&]() { state = state*1664525u+1013904223u; return float(state>>8)/16777216.f; };
    for (int i=0; i<count; ++i) {
        const float bx=random(), by=random(), phase=random()*6.2831853f;
        const float speed=0.7f+random()*1.3f;
        const float dx=std::sin(time*speed+phase)*0.06f+std::sin(time*speed*1.7f+phase)*0.025f;
        const float dy=std::cos(time*speed*0.83f+phase)*0.06f+std::sin(time*speed*1.3f+phase)*0.025f;
        const float px=bx+dx-std::floor(bx+dx), py=by+dy-std::floor(by+dy);
        const float size=1.f+random();
        vita2d_draw_rectangle(x+2+px*(w-5), y+2+py*(h-5), size, size, RGBA8(255,255,255,100+int(random()*100)));
    }
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

// Only the focused setting scrolls. Keep each column clipped, including the outer list viewport.
struct SettingsTextMarquee {
    std::string text;
    int key = -1, measured_width = 0, cell_width = 0;
    float scale = 0;
    unsigned font_revision = 0;
    int64_t started = 0, last_draw = 0;

    int offset(vita2d_pgf* font, const std::string& value, int cell_key, int width, float text_scale, int64_t now) {
        const unsigned revision = rich_text_revision();
        if (key != cell_key || text != value || cell_width != width || scale != text_scale ||
            font_revision != revision || now < last_draw || now - last_draw > 1000000) {
            key = cell_key; text = value; cell_width = width; scale = text_scale;
            font_revision = revision;
            measured_width = rich_text_width(font, text_scale, value.c_str());
            started = now;
        }
        last_draw = now;
        const int overflow = std::max(0, measured_width - width);
        if (!overflow) return 0;
        constexpr int64_t pause = 1000000, pixels_per_second = 36;
        const int64_t travel = (int64_t)overflow * 1000000 / pixels_per_second;
        const int64_t phase = (now - started) % (2 * (pause + travel));
        if (phase < pause) return 0;
        if (phase < pause + travel) return (int)((phase - pause) * pixels_per_second / 1000000);
        if (phase < 2 * pause + travel) return overflow;
        return overflow - (int)((phase - 2 * pause - travel) * pixels_per_second / 1000000);
    }
};

static void draw_settings_text(vita2d_pgf* font, SettingsTextMarquee& marquee, int key, bool focused,
                               int x, int top, int width, int height, int baseline, const std::string& text,
                               unsigned int color, float scale, int64_t now) {
    if (width <= 0 || height <= 0) return;
    const int enabled = vita2d_get_clipping_enabled();
    int left = 0, upper = 0, right = 960, lower = 544;
    if (enabled) vita2d_get_clip_rectangle(&left, &upper, &right, &lower);
    const int x0 = std::max(x, left), y0 = std::max(top, upper);
    const int x1 = std::min(x + width, right), y1 = std::min(top + height, lower);
    if (x0 >= x1 || y0 >= y1) return;
    const int offset = focused ? marquee.offset(font, text, key, width, scale, now) : 0;
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(x0, y0, x1, y1);
    draw_text(font, x - offset, baseline, focused ? text : fit_text(font, text, width, scale), color, scale);
    if (enabled) vita2d_set_clip_rectangle(left, upper, right, lower);
    else vita2d_disable_clipping();
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

// About uses separate localized blocks, so headings, risk labels and inline links
// retain their hierarchy after wrapping. Geometry is shared by drawing and input.
struct AboutLine {
    std::string text;
    int y = 0;
    float scale = 0.68f;
    bool bold = false, heading = false;
};
struct AboutLayout {
    std::vector<AboutLine> lines;
    int links_y = 0, height = 0;
    int link_y(int index) const { return links_y + (index == 0 ? 0 : 164); }
    int maximum() const { return std::max(0, height - 414); }
    bool link_visible(int index, int offset) const {
        const int y = link_y(index) - offset;
        return y + 60 > 0 && y < 414;
    }
    int hit_link(int x, int y, int offset) const {
        if (x < 40 || x >= 700 || y < 76 || y >= 490) return -1;
        const int content_y = y - 76 + offset;
        for (int i = 0; i < 2; ++i)
            if (content_y >= link_y(i) && content_y < link_y(i) + 60) return i;
        return -1;
    }
};
static const AboutLayout& about_layout(vita2d_pgf* font) {
    static AboutLayout layout;
    static std::string language;
    static unsigned revision = 0;
    static int probe = -1;
    const int current_probe = rich_text_width(font, 0.68f, "VitaGram");
    if (language == i18n::current() && revision == rich_text_revision() && probe == current_probe && layout.height)
        return layout;
    language = i18n::current(); revision = rich_text_revision(); probe = current_probe;
    layout = AboutLayout{};
    int y = 10;
    auto heading = [&](const std::string& text, float scale = 0.80f) {
        layout.lines.push_back({text, y, scale, true, true}); y += 42;
    };
    auto paragraph = [&](const std::string& text, bool bold = false) {
        for (const auto& line : wrap_text(font, text, 856)) {
            layout.lines.push_back({line, y, 0.68f, bold, false}); y += 28;
        }
        y += 16;
    };
    heading(tr("about.brand"), 0.90f);
    layout.lines.push_back({tr("about.version", {VITAGRAM_VERSION}), y, 0.55f, false, false}); y += 40;
    paragraph(tr("about.description"));
    paragraph(tr("about.development"));
    heading(tr("about.privacy_title"));
    paragraph(tr("about.privacy_servers"));
    paragraph(tr("about.privacy_logs"));
    paragraph(tr("about.privacy_connections"));
    heading(tr("about.security_title"));
    paragraph(tr("about.security_intro"));
    const char* titles[] = {"about.risk_data_title", "about.risk_encryption_title", "about.risk_temporary_title",
                            "about.risk_proxy_title", "about.risk_updates_title"};
    const char* bodies[] = {"about.risk_data", "about.risk_encryption", "about.risk_temporary",
                            "about.risk_proxy", "about.risk_updates"};
    for (int i = 0; i < 5; ++i) {
        paragraph(tr(titles[i]), true);
        y -= 10;
        paragraph(tr(bodies[i]));
    }
    paragraph(tr("about.platform_risk"));
    heading(tr("about.author_title"));
    layout.links_y = y; y += 244;
    heading(tr("about.license_title"));
    paragraph(tr("about.license_mit"));
    paragraph(tr("about.license_warranty"));
    heading(tr("about.third_party_title"));
    paragraph(tr("about.third_party_core"));
    paragraph(tr("about.third_party_media"));
    paragraph(tr("about.third_party_fonts"));
    paragraph(tr("about.third_party_vhbb"));
    paragraph(tr("about.third_party_notices"));
    layout.height = y + 8;
    return layout;
}
static constexpr const char* kAboutProjectUrl = "https://github.com/ing156/VitaGram";
static constexpr const char* kAboutAuthorUrl = "https://t.me/Ing156";
static const QrCode& about_project_qr() {
    static const QrCode qr = [] { QrCode result; qr_encode(kAboutProjectUrl, result); return result; }();
    return qr;
}
static void about_triangle(float x1, float y1, float x2, float y2, float x3, float y3, unsigned int color) {
    // GPU vertices must outlive submission; vita2d's frame pool owns their lifetime.
    auto* vertices = static_cast<vita2d_color_vertex*>(vita2d_pool_memalign(3 * sizeof(vita2d_color_vertex), 4));
    if (!vertices) return;
    vertices[0] = {x1, y1, 0.5f, color};
    vertices[1] = {x2, y2, 0.5f, color};
    vertices[2] = {x3, y3, 0.5f, color};
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, vertices, 3);
}
// Native vector marks stay crisp in either theme without additional textures.
static void draw_about_link_icon(int index, float x, float y, unsigned int accent) {
    if (index == 0) {
        const unsigned int orange = RGBA8(240, 80, 51, 255), white = RGBA8(255, 255, 255, 255);
        about_triangle(x + 14, y, x + 28, y + 14, x, y + 14, orange);
        about_triangle(x, y + 14, x + 28, y + 14, x + 14, y + 28, orange);
        for (int i = 0; i < 2; ++i) {
            vita2d_draw_line(x + 10 + i, y + 8, x + 10 + i, y + 21, white);
            vita2d_draw_line(x + 10, y + 9 + i, x + 20, y + 17 + i, white);
        }
        vita2d_draw_fill_circle(x + 10, y + 9, 2.5f, white);
        vita2d_draw_fill_circle(x + 10, y + 21, 2.5f, white);
        vita2d_draw_fill_circle(x + 20, y + 17, 2.5f, white);
    } else if (!ui_skin::icon(ui_skin::Icon::Send, x, y, 28)) {
        about_triangle(x, y + 11, x + 28, y, x + 10, y + 18, accent);
        about_triangle(x + 10, y + 18, x + 28, y, x + 21, y + 28, accent);
    }
}

static const radial::Geometry& ui_geometry() {
    static const radial::Geometry geometry(radial::Config{});
    return geometry;
}

static void rounded_rect(float x, float y, float w, float h, unsigned int color) {
    // One contour avoids repeated alpha blending where rectangles and circles overlap.
    radial::rounded_box(ui_geometry(), x, y, w, h, std::min(10.f, std::min(w, h) / 2), color);
}

// Themed PNG when the skin provides it, otherwise the original filled shape.
static void skin_box(ui_skin::Slice slice, float x, float y, float w, float h, unsigned int fallback, bool rounded = false) {
    if (ui_skin::nine(slice, x, y, w, h)) return;
    if (rounded) rounded_rect(x, y, w, h, fallback); else vita2d_draw_rectangle(x, y, w, h, fallback);
}
static void skin_row(float x, float y, float w, float h, bool focused, unsigned int fallback, bool rounded = false) {
    skin_box(focused ? ui_skin::Slice::RowFocused : ui_skin::Slice::Row, x, y, w, h, fallback, rounded);
}
// Same, for surfaces that follow the shared opacity setting (chat list, settings, message cards).
static void skin_card(float x, float y, float w, float h, bool focused, unsigned int fallback, bool rounded = false) {
    if (ui_skin::nine(focused ? ui_skin::Slice::RowFocused : ui_skin::Slice::Row, x, y, w, h, ui_skin::card_tint(focused))) return;
    if (rounded) rounded_rect(x, y, w, h, fallback); else vita2d_draw_rectangle(x, y, w, h, fallback);
}
// Bordered list frame: one panel PNG, or the original border + fill pair.
static void skin_frame(float x, float y, float w, float h, unsigned int border, unsigned int fill) {
    if (ui_skin::nine(ui_skin::Slice::Panel, x, y, w, h)) return;
    rounded_rect(x, y, w, h, border);
    rounded_rect(x + 2, y + 2, w - 4, h - 4, fill);
}
// Opaque backdrop for a full-height screen: the wallpaper, or the original flat color.
static void skin_backdrop(float y, float h, unsigned int fallback) {
    if (!ui_skin::background(y, h)) vita2d_draw_rectangle(0, y, 960, h, fallback);
}
// Progress/level bar: themed track and fill, or the original flat bars. `fraction` is 0..1.
static void skin_bar(float x, float y, float w, float h, float fraction, unsigned int track, unsigned int fill) {
    fraction = std::max(0.f, std::min(1.f, fraction));
    if (ui_skin::nine(ui_skin::Slice::ProgressTrack, x, y, w, h)) {
        if (fraction * w >= 1) ui_skin::nine(ui_skin::Slice::ProgressFill, x, y, w * fraction, h);
        return;
    }
    vita2d_draw_rectangle(x, y, w, h, track);
    if (fraction > 0) vita2d_draw_rectangle(x, y, w * fraction, h, fill);
}

// 18 px strip below the 36 px header with an equal 6 px gap above and below (content starts at 66).
static constexpr int kSearchBarShift = ui_layout::search_shift;
static constexpr int kFolderStep = ui_layout::folder_step;
static constexpr int kStripTop = ui_layout::strip_top;
static void skin_strip(unsigned int fallback) {
    // Same x and width as the main list rows (10..950).
    if (!ui_skin::nine(ui_skin::Slice::Panel, ui_layout::list_left, kStripTop, ui_layout::list_width, ui_layout::strip_height))
        vita2d_draw_rectangle(ui_layout::list_left, kStripTop, ui_layout::list_width, ui_layout::strip_height, fallback);
}

// Search field below the status strip (6 px gaps around it); tapping it opens the keyboard.
static void search_bar(vita2d_pgf* font, const std::string& text, unsigned int muted, unsigned int fallback, bool focused = false) {
    skin_row(ui_layout::list_left, ui_layout::content_top, ui_layout::list_width, ui_layout::search_height, focused, fallback, true);
    ui_skin::icon(ui_skin::Icon::Search, 30, 73, 22);
    draw_text(font, 60, 91, fit_text(font, text, 860, 0.55f), muted, 0.55f);
}

// Stand-in drawn while an avatar texture is missing, in the same shape as the masked image.
static void draw_avatar_placeholder(float x, float y, float size, unsigned int color, int shape) {
    if (shape == 1) { vita2d_draw_fill_circle(x + size / 2, y + size / 2, size / 2, color); return; }
    const float r = size * 0.22f;
    vita2d_draw_rectangle(x + r, y, size - 2 * r, size, color);
    vita2d_draw_rectangle(x, y + r, size, size - 2 * r, color);
    vita2d_draw_fill_circle(x + r, y + r, r, color);
    vita2d_draw_fill_circle(x + size - r, y + r, r, color);
    vita2d_draw_fill_circle(x + r, y + size - r, r, color);
    vita2d_draw_fill_circle(x + size - r, y + size - r, r, color);
}

// Telegram's deleted-account avatar: a white ghost on a blue background.
static void draw_deleted_avatar(float x, float y, float size, int shape) {
    const unsigned int blue = RGBA8(64,174,210,255), white = RGBA8(255,255,255,255);
    draw_avatar_placeholder(x, y, size, blue, shape);
    auto px = [&](float v) { return x + size * v; };
    auto py = [&](float v) { return y + size * v; };
    vita2d_draw_fill_circle(px(0.5f), py(0.40f), size * 0.22f, white);
    vita2d_draw_rectangle(px(0.28f), py(0.40f), size * 0.44f, size * 0.29f, white);
    about_triangle(px(0.29f), py(0.43f), px(0.17f), py(0.76f), px(0.5f), py(0.69f), white);
    about_triangle(px(0.71f), py(0.43f), px(0.83f), py(0.76f), px(0.5f), py(0.69f), white);
    for (float center : {0.27f, 0.5f, 0.73f})
        vita2d_draw_fill_circle(px(center), py(0.71f), size * 0.085f, white);
    vita2d_draw_fill_circle(px(0.42f), py(0.39f), size * 0.04f, blue);
    vita2d_draw_fill_circle(px(0.58f), py(0.39f), size * 0.04f, blue);
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
static float chat_corner_bottom = 544;
static void draw_corner_ring(bool right, bool dark, float glow) {
    const float R = 66, deg = 3.14159265f / 180.0f, cx = right ? 960.f : 0.f;
    vita2d_draw_fill_circle(cx, chat_corner_bottom, R + 5, RGBA8(255, 255, 255, dark ? 6 : 10));
    vita2d_draw_fill_circle(cx, chat_corner_bottom, R + 3, RGBA8(255, 255, 255, dark ? 9 : 14));
    const int steps = 18;
    vita2d_draw_fill_circle(cx, chat_corner_bottom, R, RGBA8(30, 34, 40, 150));
    for (int i = 1; i <= steps; ++i) {
        const float t = (float)i / steps;
        const unsigned int c = mix_color(RGBA8(62, 68, 78, 255), RGBA8(150, 158, 170, 255), t);
        vita2d_draw_fill_circle(cx, chat_corner_bottom, R * (1.0f - 0.94f * t), (c & 0x00FFFFFFu) | (14u << 24));
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
            vita2d_draw_line(cx + radius * std::cos(a * deg), chat_corner_bottom + radius * std::sin(a * deg),
                             cx + radius * std::cos((a + 2) * deg), chat_corner_bottom + radius * std::sin((a + 2) * deg),
                             RGBA8(255, 255, 255, (unsigned int)std::min(255.f, alpha)));
        }
    }
    for (int a = from + 2; a < from + 86; a += 3)
        vita2d_draw_line(cx + (R - 7) * std::cos(a * deg), chat_corner_bottom + (R - 7) * std::sin(a * deg),
                         cx + (R - 7) * std::cos((a + 3) * deg), chat_corner_bottom + (R - 7) * std::sin((a + 3) * deg), RGBA8(255, 255, 255, 22));
}

static void draw_attach_button(bool dark, float glow = 0, float opacity = 1.f) {
    if (ui_skin::corner_button(false, glow, opacity, chat_corner_bottom)) return;
    if (opacity < 0.5f) return;
    draw_corner_ring(false, dark, glow);
    draw_paperclip(26, chat_corner_bottom-25, 1.55f, RGBA8(0, 0, 0, 90));
    draw_paperclip(25, chat_corner_bottom-26, 1.55f, RGBA8(244, 247, 251, 255));
}

// Three dots, as on the original system menu button.
static void draw_menu_button(bool dark, float glow = 0, float opacity = 1.f) {
    if (ui_skin::corner_button(true, glow, opacity, chat_corner_bottom)) return;
    if (opacity < 0.5f) return;
    draw_corner_ring(true, dark, glow);
    for (int i = 0; i < 3; ++i) {
        vita2d_draw_fill_circle(924.f + i * 11 + 1, chat_corner_bottom-23, 3.2f, RGBA8(0, 0, 0, 90));
        vita2d_draw_fill_circle(924.f + i * 11, chat_corner_bottom-24, 3.2f, RGBA8(244, 247, 251, 255));
    }
}

// One entry of the right-hand context menu: label key (text carries the button icon), the physical
// button it stands for, or a special action.
struct ChatMenuItem { const char* label; unsigned int button; int special; int icon = -1; std::string text = {}; };
// Same geometry as the attach menu: 40 px row pitch, 36 px focus bars inset 4 px, first row 4 px below the frame top.
static constexpr int kMenuRowHeight = ui_layout::menu_row, kMenuX = ui_layout::menu_left, kMenuWidth = ui_layout::menu_width,
    kMenuPad = ui_layout::menu_pad,
    kMenuFocusHeight = ui_layout::menu_focus_height;
static int kMenuBottom = ui_layout::menu_bottom;
static chat_view::Layout history_view;
static int history_menu_rows() { return std::min(history_view.menuRows(), (kMenuBottom-history_view.top-2*kMenuPad-4)/kMenuRowHeight); }
struct PostReaction { std::string text; int width; bool chosen; int64_t custom_emoji_id; };
// A link inside one wrapped line: UTF-8 byte range of PostLine::text.
struct LineLink { int begin, end; std::string url; };
struct PostLine {
    std::string text, url;
    std::vector<StyleSpan> styles;
    std::vector<LineLink> links;   // inline links; url holds the first one for the keyboard
    bool transcript = false;       // belongs to the transcription block (revealed with an animation)
    bool comments = false;         // the comments row of a channel post (text holds its label)
    std::vector<PostReaction> reactions;
    int quote_width = 0, quote_pitch = 0;
    bool quote_first = false, quote_last = false;
    PostLine(std::string value, std::string link) : text(std::move(value)), url(std::move(link)) {}
};

// Distributes message link spans over the wrapped lines. Wrapping only re-flows whitespace, so the
// n-th non-space byte of the text is the n-th non-space byte of the concatenated lines.
static std::vector<std::vector<LineLink>> map_links_to_lines(const std::string& text, const std::vector<std::string>& lines,
                                                              const std::vector<LinkSpan>& spans) {
    std::vector<std::vector<LineLink>> result(lines.size());
    if (spans.empty()) return result;
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    std::vector<int> before(text.size() + 1, 0);   // non-space bytes in text[0, i)
    for (size_t i = 0; i < text.size(); ++i) before[i + 1] = before[i] + (space(text[i]) ? 0 : 1);
    int line_start = 0;
    for (size_t k = 0; k < lines.size(); ++k) {
        const std::string& line = lines[k];
        int length = 0;
        for (char c : line) if (!space(c)) ++length;
        for (const auto& span : spans) {
            if (span.begin < 0 || span.end > (int)text.size() || span.end <= span.begin) continue;
            const int from = std::max(before[span.begin], line_start), to = std::min(before[span.end], line_start + length);
            if (to <= from) continue;
            int first = -1, last = -1, seen = line_start;
            for (size_t i = 0; i < line.size(); ++i) {
                if (space(line[i])) continue;
                if (seen == from) first = (int)i;
                if (seen == to - 1) last = (int)i + 1;
                ++seen;
            }
            if (first >= 0 && last > first) result[k].push_back({first, last, span.url});
        }
        line_start += length;
    }
    return result;
}

static std::vector<std::vector<StyleSpan>> map_styles(const MessageItem& message, const std::vector<std::string>& lines) {
    std::vector<LinkSpan> spans;
    for (const auto& span : message.styles) spans.push_back({span.begin, span.end, std::to_string(span.style)});
    const auto mapped = map_links_to_lines(message.text, lines, spans);
    std::vector<std::vector<StyleSpan>> result(lines.size());
    for (size_t i = 0; i < lines.size(); ++i)
        for (const auto& span : mapped[i]) result[i].push_back({span.begin, span.end, (unsigned)std::stoul(span.url)});
    return result;
}

static bool has_block_quote(const MessageItem& message) {
    for (const auto& span : message.styles) if (span.style & TextQuote) return true;
    return false;
}

static void layout_block_quotes(vita2d_pgf* font, const MessageItem& message,
                                 const std::vector<std::string>& wrapped, std::vector<PostLine>& lines, int pitch) {
    std::vector<LinkSpan> quotes;
    for (size_t i = 0; i < message.styles.size(); ++i) {
        const auto& span = message.styles[i];
        if (span.style & TextQuote) quotes.push_back({span.begin, span.end, std::to_string(i)});
    }
    const auto mapped = map_links_to_lines(message.text, wrapped, quotes);
    std::vector<QuoteRow> rows(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) if (!mapped[i].empty()) {
        rows[i].group = std::stoi(mapped[i][0].url);
        rows[i].width = rich_text_width(font, 0.68f, lines[i].text.c_str());
    }
    layout_quote_rows(rows);
    for (size_t i = 0; i < lines.size(); ++i) if (rows[i].group >= 0) {
        lines[i].quote_width = rows[i].width; lines[i].quote_pitch = pitch;
        lines[i].quote_first = rows[i].first; lines[i].quote_last = rows[i].last;
    }
}

static int scaled_px(int value);

static void draw_formatted_line(vita2d_pgf* font, int x, int baseline, const PostLine& line,
                                unsigned ink, unsigned accent, bool revealed) {
    if (line.quote_width) {
        const int top = baseline - scaled_px(18) - 2, height = line.quote_pitch;
        const unsigned fill = scale_alpha(accent, 0.16f);
        const int upper = line.quote_first ? 4 : 0, lower = line.quote_last ? 4 : 0;
        vita2d_draw_rectangle(x - 4, top + upper, line.quote_width, height - upper - lower, fill);
        for (int row = 0; row < 4; ++row) {
            const int inset = row == 0 ? 3 : row == 1 ? 1 : 0;
            if (upper) vita2d_draw_rectangle(x - 4 + inset, top + row, line.quote_width - 2 * inset, 1, fill);
            if (lower) vita2d_draw_rectangle(x - 4 + inset, top + height - row - 1, line.quote_width - 2 * inset, 1, fill);
        }
        vita2d_draw_rectangle(x - 4, top + (line.quote_first ? 3 : 0), 3,
            height - (line.quote_first ? 3 : 0) - (line.quote_last ? 3 : 0), accent);
        if (line.quote_first) draw_text(font, x + line.quote_width - 22, baseline, "❞", accent, 0.55f);
        x += 10;
    }
    std::vector<int> cuts{0, (int)line.text.size()};
    for (const auto& span : line.styles) { cuts.push_back(span.begin); cuts.push_back(span.end); }
    for (const auto& span : line.links) { cuts.push_back(span.begin); cuts.push_back(span.end); }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    for (size_t i = 1; i < cuts.size(); ++i) {
        int begin = cuts[i-1], end = cuts[i];
        unsigned style = 0; bool link = false;
        for (const auto& span : line.styles) if (span.begin <= begin && span.end >= end) style |= span.style;
        for (const auto& span : line.links) if (span.begin <= begin && span.end >= end) link = true;
        const auto part = line.text.substr(begin, end-begin);
        const int width = rich_text_width(font, 0.68f, part.c_str());
        const unsigned color = link || (!line.url.empty() && line.links.empty()) ? accent : ink;
        if ((style & TextSpoiler) && !revealed) {
            vita2d_draw_rectangle(x, baseline-scaled_px(16), width, scaled_px(18), ink);
        } else {
            if (style & TextCode) vita2d_draw_rectangle(x, baseline-scaled_px(18), width, scaled_px(22), scale_alpha(accent, 0.18f));
            TextFont::style = style & 3;
            draw_text(font, x, baseline, part, color, 0.68f);
            TextFont::style = 0;
            if (link || (style & TextUnderline)) vita2d_draw_rectangle(x, baseline+3, width, 1, color);
            if (style & TextStrike) vita2d_draw_rectangle(x, baseline-scaled_px(7), width, 1, color);
        }
        x += width;
    }
}

// Link under a horizontal touch position; falls back to the line's first link, as tapping a line always did.
static std::string post_link_at(vita2d_pgf* font, const PostLine& line, int touch_x, int x = 38) {
    int cursor = x + (line.quote_width ? 10 : 0), position = 0;
    for (const auto& link : line.links) {
        cursor += rich_text_width(font, 0.68f, line.text.substr(position, link.begin - position).c_str());
        const int width = rich_text_width(font, 0.68f, line.text.substr(link.begin, link.end - link.begin).c_str());
        if (touch_x >= cursor && touch_x < cursor + width) return link.url;
        cursor += width; position = link.end;
    }
    return line.url;
}

static int post_pitch();
static void draw_comments_row(vita2d_pgf* font, float x, float y, float w, float h, const std::string& label,
                              unsigned int ink, unsigned int accent);
static void draw_post_line(vita2d_pgf* font, int x, int baseline, const PostLine& line,
                           unsigned int ink, unsigned int accent, TelegramClient& tg,
                           TextureCache& textures, bool pause_media, bool revealed) {
    if (line.comments) {
        draw_comments_row(font, (float)x - 8, (float)baseline - 19, history_view.width(900), (float)post_pitch() - 2, line.text, ink, accent);
        return;
    }
    if (line.reactions.empty()) {
        draw_formatted_line(font, x, baseline, line, ink, accent, revealed);
        return;
    }
    if (!pause_media) tg.ensureMessageReactionMedia();
    const unsigned reaction_ink = ui_color::on(accent);
    for (const auto& reaction : line.reactions) {
        if (reaction.chosen) rounded_rect(x, baseline-18, reaction.width, 22, accent);
        int text_x=x+7;
        if (reaction.custom_emoji_id) {
            auto icon=textures.get(tg.customReactionIcon(reaction.custom_emoji_id));
            if (icon) draw_texture_fit(icon,x+7,baseline-18,20,20);
            else vita2d_draw_fill_circle(x+17,baseline-8,4,
                reaction.chosen ? reaction_ink : ink);
            text_x+=24;
        }
        draw_text(font, text_x, baseline, reaction.text, reaction.chosen ? reaction_ink : ink, 0.68f);
        x += reaction.width + 6;
    }
}

// ---- media controls (voice waveform, video control bar, video-note ring) ----
struct Rect4 {
    bool on = false;
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return on && px >= x && px < x + w && py >= y && py < y + h; }
};
#if VITAGRAM_WEBKIT
static constexpr Rect4 webapp_primary_rect{true, 330, 500, 350, 42};
static constexpr Rect4 webapp_back_rect{true, 32, 500, 268, 42};
static constexpr Rect4 webapp_close_rect{true, 740, 500, 172, 42};
#endif
static std::string clock_text(int64_t us) {
    const int total = (int)std::max<int64_t>(0, us / 1000000);
    char text[16]; std::snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
    return text;
}
static void draw_audio_control(audio_bar::Control control, float cx, float cy, bool playing, int volume, unsigned color) {
    using namespace audio_bar;
    auto line = [&](float x1, float y1, float x2, float y2) { vita2d_draw_line(cx+x1, cy+y1, cx+x2, cy+y2, color); };
    if (control == Previous || control == Next) {
        const float direction = control == Previous ? -1.f : 1.f;
        about_triangle(cx-7*direction,cy-9,cx+6*direction,cy,cx-7*direction,cy+9,color);
        vita2d_draw_rectangle(cx+(direction<0 ? -10 : 8),cy-9,3,18,color);
    } else if (control == Play) {
        if (playing) { vita2d_draw_rectangle(cx-7,cy-10,5,20,color); vita2d_draw_rectangle(cx+3,cy-10,5,20,color); }
        else about_triangle(cx-6,cy-10,cx+9,cy,cx-6,cy+10,color);
    } else if (control == Volume || control == Mute) {
        vita2d_draw_rectangle(cx-11,cy-4,6,8,color);
        line(-5,-4,2,-10); line(2,-10,2,10); line(2,10,-5,4);
        if (volume) { line(6,-5,9,0); line(9,0,6,5); line(9,-9,14,0); line(14,0,9,9); }
        else { line(7,-4,14,4); line(7,4,14,-4); }
    } else if (control == Shuffle) {
        line(-12,-7,-7,-7); line(-7,-7,7,7); line(7,7,12,7);
        line(-12,7,-7,7); line(-7,7,-2,2); line(2,-2,7,-7); line(7,-7,12,-7);
        line(8,-11,12,-7); line(12,-7,8,-3); line(8,3,12,7); line(12,7,8,11);
    } else if (control == Repeat) {
        line(-10,3,-10,-7); line(-10,-7,11,-7); line(7,-11,11,-7); line(11,-7,7,-3);
        line(10,-3,10,7); line(10,7,-11,7); line(-7,3,-11,7); line(-11,7,-7,11);
    } else if (control == Close) { line(-7,-7,7,7); line(-7,7,7,-7); }
}
static MediaLoadView video_load_view(const MessageItem& message, bool decoder_ready = false) {
    const auto file = message.animation_stream ? message.animation_stream->snapshot() : download_stream::State::Snapshot{};
    return MediaLoadView::make(std::max(message.animation_size, file.size),
        std::max(message.animation_downloaded, file.prefix), !message.animation_path.empty() || file.complete, decoder_ready);
}
static std::string media_byte_count(int64_t bytes) {
    if (bytes < 1000000) return tr("unit.kb", {std::to_string(std::max<int64_t>(0, bytes) / 1000)});
    char value[32]; std::snprintf(value, sizeof(value), "%.1f", bytes / 1000000.0);
    return tr("unit.mb", {value});
}
// Use the same panel, icon atlas and progress slices as the rest of the UI.
// Download status stays visible even when fullscreen playback controls fade out.
static void draw_media_loading(vita2d_pgf* font, float x, float y, float w, const MediaLoadView& view,
                               int64_t now, unsigned accent, unsigned ink, unsigned muted,
                               const std::string& opening = "", float opacity = 1.f) {
    constexpr float h = 64;
    if (opacity <= 0.01f) return;
    // Fades together with the fullscreen controls: every color and skin tint carries the opacity.
    const unsigned tint = RGBA8(255, 255, 255, (unsigned)(255.f * opacity));
    accent = scale_alpha(accent, opacity); ink = scale_alpha(ink, opacity); muted = scale_alpha(muted, opacity);
    if (!ui_skin::nine(ui_skin::Slice::Panel, x, y, w, h, tint))
        vita2d_draw_rectangle(x, y, w, h, scale_alpha(ui_skin::surface_color(RGBA8(24,40,58,240)), opacity));
    if (!ui_skin::icon(ui_skin::Icon::Download, x + 16, y + 12, 24, tint)) {
        vita2d_draw_line(x + 28, y + 12, x + 28, y + 28, accent);
        vita2d_draw_line(x + 22, y + 22, x + 28, y + 28, accent);
        vita2d_draw_line(x + 34, y + 22, x + 28, y + 28, accent);
        vita2d_draw_line(x + 20, y + 33, x + 36, y + 33, accent);
    }
    const std::string title = view.downloading || opening.empty() ? tr("video.loading") : opening;
    const std::string amount = !view.downloading ? "" : view.size > 0
        ? std::to_string(view.percent) + "%  " + media_byte_count(std::min(view.downloaded, view.size)) + " / " + media_byte_count(view.size)
        : media_byte_count(view.downloaded);
    const float amount_width = rich_text_width(font, 0.48f, amount.c_str());
    draw_text(font, x + 52, y + 28, fit_text(font, title, w - 84 - amount_width, 0.56f), ink, 0.56f);
    if (!amount.empty()) draw_text(font, x + w - 16 - amount_width, y + 28, amount, muted, 0.48f);
    const float bx = x + 16, bw = w - 32, by = y + 46;
    const bool determinate = view.downloading && view.percent >= 0;
    const float fraction = determinate ? std::max(0.f, std::min(1.f, view.fraction)) : 0.f;
    if (ui_skin::nine(ui_skin::Slice::ProgressTrack, bx, by, bw, 8, tint)) {
        if (fraction * bw >= 1) ui_skin::nine(ui_skin::Slice::ProgressFill, bx, by, bw * fraction, 8, tint);
    } else {
        vita2d_draw_rectangle(bx, by, bw, 8, scale_alpha(muted, 0.4f));
        if (fraction > 0) vita2d_draw_rectangle(bx, by, bw * fraction, 8, accent);
    }
    if (!determinate) {
        const float pulse_x = bx + (bw - bw * 0.2f) * (0.5f + 0.5f * std::sin(now / 400000.0));
        if (!ui_skin::nine(ui_skin::Slice::ProgressFill, pulse_x, by, bw * 0.2f, 8, tint))
            vita2d_draw_rectangle(pulse_x, by, bw * 0.2f, 8, accent);
    }
}
// Rotating dots over the picture while a video waits for its next picture (after a seek or for streamed bytes).
static void draw_buffering_spinner(float cx, float cy, int64_t now, unsigned accent, float radius = 40) {
    constexpr int dots = 10;
    constexpr float pi = 3.14159265f;
    vita2d_draw_fill_circle(cx, cy, radius, RGBA8(0, 0, 0, 120));
    const float head = static_cast<float>(std::fmod(now / 80000.0, dots));   // one dot per 80 ms
    for (int i = 0; i < dots; ++i) {
        const float angle = i * 2 * pi / dots - pi / 2;
        const float fresh = 1.f - std::fmod(head - i + dots, (float)dots) / dots;   // 1 at the head, fading behind it
        vita2d_draw_fill_circle(cx + radius * 0.6f * std::cos(angle), cy + radius * 0.6f * std::sin(angle), radius * 0.0625f * (1 + fresh),
                                scale_alpha(accent, 0.2f + 0.8f * fresh));
    }
}
struct ControlBar { Rect4 play, track; };
// Play/pause, progress with a knob and "elapsed / total" in one row, so nothing overlaps at any width:
// the time text takes what it needs on the right and the track fills the space between icon and time.
static ControlBar draw_control_bar(vita2d_pgf* font, float x, float y, float w, float h, int64_t position_us, int64_t duration_us,
                                   bool playing, bool seekable, float scrub, unsigned accent, unsigned ink, unsigned muted, float fade = 1.f) {
    const std::string text = clock_text(position_us) + " / " + (duration_us > 0 ? clock_text(duration_us) : std::string("--:--"));
    const int text_width = rich_text_width(font, 0.55f, text.c_str());
    const float icon_size = std::min(28.f, h - 8), cy = y + h / 2;
    const float track_x = x + 10 + icon_size + 14, track_w = std::max(20.f, w - (track_x - x) - 14 - text_width - 14);
    ControlBar bar;
    bar.play = {true, x, y, 10 + icon_size + 8, h};
    bar.track = {true, track_x - 8, y, track_w + 16, h};
    // The hidden fullscreen strip keeps the exact same touch geometry.
    if (fade <= 0.01f) return bar;
    const unsigned int white_fade = RGBA8(255, 255, 255, (int)(255.f * fade));
    accent = scale_alpha(accent, fade); ink = scale_alpha(ink, fade); muted = scale_alpha(muted, fade);
    if (!ui_skin::nine(ui_skin::Slice::Panel, x, y, w, h, scale_alpha(ui_skin::card_tint(false), fade))) {
        rounded_rect(x, y, w, h, RGBA8(0, 0, 0, (int)(150.f * fade)));
    }
    if (!ui_skin::icon(playing ? ui_skin::Icon::Pause : ui_skin::Icon::Play, x + 10, cy - icon_size / 2, icon_size, white_fade)) {
        if (playing) { vita2d_draw_rectangle(x + 14, cy - 8, 4, 16, ink); vita2d_draw_rectangle(x + 22, cy - 8, 4, 16, ink); }
        else for (int row = 0; row < 16; ++row) vita2d_draw_rectangle(x + 14, cy - 8 + row, 12 * (1.f - std::fabs(row - 7.5f) / 8.f), 1, ink);
    }
    draw_text(font, (int)(x + w - 14 - text_width), (int)(cy + 6), text, ink, 0.55f);
    const float fraction = scrub >= 0 ? scrub : duration_us > 0 ? (float)std::max<int64_t>(0, std::min(position_us, duration_us)) / duration_us : 0.f;
    const float track_y = cy - 4;
    if (ui_skin::nine(ui_skin::Slice::ProgressTrack, track_x, track_y, track_w, 8, white_fade)) {
        if (fraction * track_w >= 1) ui_skin::nine(ui_skin::Slice::ProgressFill, track_x, track_y, track_w * fraction, 8, white_fade);
    } else {
        vita2d_draw_rectangle(track_x, track_y, track_w, 8, scale_alpha(muted, 0.5f));
        vita2d_draw_rectangle(track_x, track_y, track_w * fraction, 8, accent);
    }
    if (seekable) {
        vita2d_draw_fill_circle(track_x + track_w * fraction, cy, 8, accent);
        vita2d_draw_fill_circle(track_x + track_w * fraction, cy, 4, white_fade);
    }
    return bar;
}
// Ring around a round video: one triangle strip for the track and one for the elapsed part.
static void draw_progress_ring(float cx, float cy, float radius, float width, float fraction, bool knob, unsigned track, unsigned fill) {
    constexpr int steps = 180;
    auto build = [&](int count, unsigned color) {
        // vita2d_draw_array hands the pointer straight to the GPU: the vertices must live in the per-frame
        // vita2d pool (a stack array here crashes the GPU as soon as the first frame draws the ring).
        auto* strip = static_cast<vita2d_color_vertex*>(vita2d_pool_memalign(2 * (count + 1) * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex)));
        if (!strip) return;
        for (int i = 0; i <= count; ++i) {
            const float angle = -1.57079633f + 6.28318531f * i / steps, c = std::cos(angle), sn = std::sin(angle);
            strip[2 * i] = {cx + (radius - width / 2) * c, cy + (radius - width / 2) * sn, 0.5f, color};
            strip[2 * i + 1] = {cx + (radius + width / 2) * c, cy + (radius + width / 2) * sn, 0.5f, color};
        }
        vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, strip, 2 * (count + 1));
    };
    build(steps, track);
    const int elapsed = (int)std::lround(steps * std::max(0.f, std::min(1.f, fraction)));
    if (elapsed > 0) build(elapsed, fill);
    if (knob) {
        const float angle = -1.57079633f + 6.28318531f * fraction;
        vita2d_draw_fill_circle(cx + radius * std::cos(angle), cy + radius * std::sin(angle), width + 3, fill);
        vita2d_draw_fill_circle(cx + radius * std::cos(angle), cy + radius * std::sin(angle), width - 1, RGBA8(255, 255, 255, 255));
    }
}
// Telegram-style voice waveform: bars resampled to the available width, played part in the accent colour.
static void draw_waveform(float x, float center_y, float w, float max_height, const std::vector<uint8_t>& samples, float fraction,
                          unsigned played, unsigned unplayed) {
    const int bars = std::max(1, std::min((int)samples.size(), (int)(w / 6)));
    const float pitch = w / bars, bar_w = std::max(2.f, pitch - 2.f);
    for (int b = 0; b < bars; ++b) {
        const size_t from = (size_t)b * samples.size() / bars, to = std::max(from + 1, (size_t)(b + 1) * samples.size() / bars);
        int sum = 0;
        for (size_t i = from; i < to && i < samples.size(); ++i) sum += samples[i];
        const float amplitude = (float)sum / (float)(to - from) / 31.f;
        const float h = 4.f + amplitude * (max_height - 4.f);
        vita2d_draw_rectangle(x + b * pitch, center_y - h / 2, bar_w, h, (b + 0.5f) / bars <= fraction ? played : unplayed);
    }
}

static int scaled_px(int base);
// ---- reply quote: compact rounded panel with an accent bar, sender name and a one-line preview ----
static int quote_height() { return ui_layout::quote_padding + scaled_px(ui_layout::quote_content_height); }
static void draw_reply_quote(vita2d_pgf* font, float x, float y, float w, const MessageItem& message, bool dark, unsigned ink, unsigned accent, const peer_colors::Palette& colors = {}) {
    if (colors.count) accent = peer_colors::rgba(colors.rgb[0]);
    const float h = (float)quote_height();
    // The date-chip PNG is a slightly different tone than the message card in both themes; flat fallback otherwise.
    if (!ui_skin::nine(ui_skin::Slice::DateChip, x, y, w, h, RGBA8(255, 255, 255, 235)))
        rounded_rect(x, y, w, h, dark ? RGBA8(26, 58, 92, 255) : RGBA8(222, 238, 250, 255));
    vita2d_draw_rectangle(x + 6, y + 6, 3, h - 12, accent);
    for (int stripe = 0; colors.count > 1 && stripe * 6 < h - 12; ++stripe)
        vita2d_draw_rectangle(x + 6, y + 6 + stripe * 6, 3, std::min(3.f, h - 12 - stripe * 6),
            peer_colors::rgba(colors.rgb[1 + stripe % (colors.count - 1)]));
    const int text_width = (int)w - 26;
    if (!message.reply_sender.empty())
        draw_text(font, (int)x + 16, (int)y + 6 + scaled_px(13), fit_text(font, message.reply_sender, text_width, 0.5f), accent, 0.5f);
    const std::string preview = message.reply_text.empty() && !message.reply_missing ? i18n::Text("...") : message.reply_text;
    draw_text(font, (int)x + 16, (int)(y + h - 8), fit_text(font, preview, text_width, 0.55f), message.reply_missing ? scale_alpha(ink, 0.6f) : ink, 0.55f);
}

// ---- forwarded-from header: a thin accent line, a small muted label and the source in the accent colour ----
static int forward_height() { return scaled_px(16) + 6; }
static void draw_forward_header(vita2d_pgf* font, float x, float y, float w, const MessageItem& message, unsigned muted, unsigned accent) {
    vita2d_draw_rectangle(x, y + 1, 2, (float)scaled_px(16) - 3, accent);
    const int baseline = (int)y + scaled_px(12);
    if (message.forward_source.empty()) {
        draw_text(font, (int)x + 8, baseline, fit_text(font, tr("forward.unknown"), (int)w - 10, 0.5f), accent, 0.5f);
        return;
    }
    const std::string label = tr("forward.from") + " ";
    const int label_width = rich_text_width(font, 0.45f, label.c_str());
    draw_text(font, (int)x + 8, baseline, label, muted, 0.45f);
    draw_text(font, (int)x + 8 + label_width, baseline, fit_text(font, message.forward_source, std::max(20, (int)w - 10 - label_width), 0.5f), accent, 0.5f);
}

static std::string chat_update_time(int64_t timestamp) {
    SceDateTime date{};
    SceRtcTick utc{}, local{};
    if (sceRtcSetTime_t(&date, static_cast<time_t>(timestamp)) < 0 || sceRtcGetTick(&date, &utc) < 0)
        return "--:--:--";
    const bool local_ok = sceRtcConvertUtcToLocalTime(&utc, &local) >= 0 && sceRtcSetTick(&date, &local) >= 0;
    char text[24];
    std::snprintf(text, sizeof(text), "%02u:%02u:%02u%s", (unsigned)date.hour,
        (unsigned)date.minute, (unsigned)date.second, local_ok ? "" : " UTC");
    return text;
}

static std::string message_time(const MessageItem& message, bool full = false) {
    if (message.date <= 0) return "--:--";
    SceDateTime date{};
    SceRtcTick utc{}, local{};
    if (sceRtcSetTime_t(&date, static_cast<time_t>(message.date)) < 0 || sceRtcGetTick(&date, &utc) < 0)
        return "--:--";
    bool local_ok = sceRtcConvertUtcToLocalTime(&utc, &local) >= 0 && sceRtcSetTick(&date, &local) >= 0;
    char text[96];
    const std::string edited = message.edit_date ? "✎ " + tr("message.edited") + " " : "";
    if (full) std::snprintf(text, sizeof(text), "%s%02u.%02u.%04u  %02u:%02u%s", edited.c_str(),
        (unsigned)date.day, (unsigned)date.month, (unsigned)date.year, (unsigned)date.hour, (unsigned)date.minute,
        local_ok ? "" : " UTC");
    else std::snprintf(text, sizeof(text), "%s%02u:%02u%s", edited.c_str(), (unsigned)date.hour, (unsigned)date.minute, local_ok ? "" : " UTC");
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

// Spacing follows the font-size setting: text grows relative to the default size, so line pitch
// and card heights grow with it (never shrink below the layout the 960x544 screens were drawn for).
static float g_layout_scale = 1.0f;
static void apply_font_scale(float scale) {
    rich_text_set_scale(scale);
    g_layout_scale = std::max(1.0f, scale / 1.2f);
}
static int scaled_px(int base) { return (int)std::lround(base * g_layout_scale); }
static int post_pitch() { return scaled_px(24); }          // line pitch of expanded posts
static int history_pitch() { return scaled_px(22); }       // body line pitch inside message cards
static constexpr int kCardPad = ui_layout::card_pad;
static constexpr int kTimeGap = ui_layout::time_gap;
// ---- comments of channel posts (linked discussion thread) ----
// Short form for message cards: just the count (or the invitation when there are none yet).
static std::string comments_badge_text(const MessageItem& message) {
    return message.comment_count > 0 ? std::to_string(message.comment_count) : tr("comments.none");
}
// Row of the expanded post: "Comments \xC2\xB7 N", or the invitation when there are none yet.
static std::string comments_label(const MessageItem& message) {
    return message.comment_count > 0 ? tr("comments.title") + " \xC2\xB7 " + std::to_string(message.comment_count) : tr("comments.none");
}
// Speech bubble with a tail; the icon atlas has none, so it is drawn from primitives.
static void draw_comment_bubble(float x, float y, float size, unsigned int color) {
    const float body = size * 0.74f;
    vita2d_draw_rectangle(x + 2, y, size - 4, body, color);
    vita2d_draw_rectangle(x, y + 2, size, body - 4, color);
    vita2d_draw_fill_circle(x + 2, y + 2, 2, color); vita2d_draw_fill_circle(x + size - 2, y + 2, 2, color);
    vita2d_draw_fill_circle(x + 2, y + body - 2, 2, color); vita2d_draw_fill_circle(x + size - 2, y + body - 2, 2, color);
    vita2d_draw_rectangle(x + 3, y + body, 6, 2, color);
    vita2d_draw_rectangle(x + 3, y + body + 2, 3, 2, color);
}
static constexpr int kBadgeIcon = 16, kBadgeGap = 6;
static int comments_badge_width(vita2d_pgf* font, const MessageItem& message) {
    return kBadgeIcon + kBadgeGap + rich_text_width(font, 0.52f, comments_badge_text(message).c_str());
}
// Bubble icon and count in the timestamp row, ending at `right`; returns the left edge.
static int draw_comments_badge(vita2d_pgf* font, int right, int baseline, const MessageItem& message, unsigned int ink, unsigned int accent) {
    const int left = right - comments_badge_width(font, message);
    draw_comment_bubble((float)left, (float)baseline - 13, (float)kBadgeIcon, accent);
    draw_text(font, left + kBadgeIcon + kBadgeGap, baseline, comments_badge_text(message), ink, 0.52f);
    return left;
}
// Row of the expanded post: a thin separator, bubble icon, label and a right arrow.
static void draw_comments_row(vita2d_pgf* font, float x, float y, float w, float h, const std::string& label,
                              unsigned int ink, unsigned int accent) {
    vita2d_draw_rectangle(x, y, w, 1, scale_alpha(accent, 0.55f));
    const float icon = std::min(18.f, h - 6);
    draw_comment_bubble(x + 6, y + (h - icon * 0.74f - 4) / 2 + 5, icon, accent);
    draw_text(font, (int)x + 34, (int)(y + h / 2 + 10), fit_text(font, label, (int)w - 70, 0.68f), ink, 0.68f);
    if (!ui_skin::icon(ui_skin::Icon::Next, x + w - 26, y + (h - 16) / 2 + 5, 16, accent)) {
        const float cx = x + w - 16, cy = y + h / 2 + 4;
        for (int t = 0; t < 2; ++t) {
            vita2d_draw_line(cx - 4, cy - 6 + t, cx + 2, cy + t, accent);
            vita2d_draw_line(cx + 2, cy + t, cx - 4, cy + 6 + t, accent);
        }
    }
}
// Height of one message card including the gap below it; shared by drawing and hit testing.
static int link_preview_height(const MessageItem& message) {
    if (message.preview_url.empty()) return 0;
    return 16 + history_pitch() * (1 + (message.preview_title.empty() ? 0 : 2) +
        (message.preview_description.empty() ? 0 : 2)) + (message.media_file_id > 0 ? 138 : 0);
}
static void draw_link_preview(vita2d_pgf* font, const MessageItem& message, TextureCache& textures,
                              int x, int y, int width, unsigned int ink, unsigned int accent) {
    const int height = link_preview_height(message);
    if (!height || y >= 544 || y + height < 60) return;
    rounded_rect(x, y, width, height, scale_alpha(accent, 0.12f));
    vita2d_draw_rectangle(x, y, 3, height, accent);
    int baseline = y + history_pitch();
    draw_text(font, x + 12, baseline, fit_text(font, message.preview_site.empty() ? message.preview_url : message.preview_site,
        width - 24, 0.55f), accent, 0.55f);
    for (const auto* text : {&message.preview_title, &message.preview_description}) {
        if (text->empty()) continue;
        auto lines = wrap_text(font, clip(*text, 2048), width - 24);
        if (lines.size() > 2) {
            lines.resize(2);
            lines.back() = fit_text(font, lines.back() + "...", width - 24, 0.68f);
        }
        for (size_t row = 0; row < lines.size(); ++row)
            draw_text(font, x + 12, baseline + (row + 1) * history_pitch(), lines[row], ink, 0.68f);
        baseline += 2 * history_pitch();
    }
    if (message.media_file_id > 0) {
        auto* image = textures.get(message.media_path);
        if (image) draw_texture_fit(image, x + 12, baseline + 8, width - 24, 130);
        else draw_text(font, x + 12, baseline + 50, "...", accent, 0.68f);
    }
}
static bool message_has_media(const MessageItem& message) {
    return message.preview_url.empty() && (!message.photos.empty() || message.media_file_id > 0 || message.animation);
}
// Body lines shown in a card (at most two), wrapped exactly as the card draws them.
static std::vector<std::string> message_body_lines(vita2d_pgf* font, const MessageItem& message) {
    auto lines = wrap_text(font, clip(message.text, 2048), history_view.bodyWidth() - (message_has_media(message) ? 108 : 0) - (has_block_quote(message) ? 36 : 0));
    if (lines.size() > 2) {
        lines.resize(2);
        lines.back() = fit_text(font, lines.back() + "...", history_view.bodyWidth() - (message_has_media(message) ? 108 : 0) - (has_block_quote(message) ? 36 : 0), 0.68f);
    }
    return lines;
}
// Baseline (relative to the card content origin) of the label row, or of where the timestamp row starts.
static int message_body_end(const MessageItem& message, size_t line_count) {
    const int text_end = 20 + (history_pitch() - 22) + ((int)std::max<size_t>(1, line_count) - 1) * history_pitch();
    // Voice notes show a play button and waveform above the label row, like a thumbnail does.
    return std::max(text_end + 19, message_has_media(message) ? 61 : (message.voice && !message.audio) ? 72 : 0);
}
static int message_time_row(const MessageItem& message, size_t line_count) {
    return message_body_end(message, line_count) + link_preview_height(message) + (message.media_label.empty() ? 0 : 22) + kTimeGap;
}
// The card shrinks to its text: one-line messages are short, two-line ones taller.
static int message_height(vita2d_pgf* font, const MessageItem& message, bool group) {
    if (message.service_message) return 36;
    return message_time_row(message, message_body_lines(font, message).size()) + kCardPad + 17 +
        bot_buttons::preview_height(message.buttons) +
        (group && !message.outgoing ? std::max(20, scaled_px(20)) : 0) + (message.reply_to_id ? quote_height() + ui_layout::quote_gap : 0) + (message.forwarded ? forward_height() : 0);
}

static int history_bottom = 544;
static HistoryScroll history_scroll;
static bool history_join(const std::vector<MessageItem>& messages, int i) {
    return i > 0 && i < (int)messages.size() && chat_bubble::joins(messages[i-1], messages[i]) &&
        message_day(messages[i-1]) == message_day(messages[i]);
}
struct HistoryRow {
    int index, top, height, day_top;
    std::string day;
    bool before, after;
    int gap() const { return after ? 2 : 6; }
};
static int history_message_height(vita2d_pgf* font, const std::vector<MessageItem>& messages, int i, bool group) {
    return message_height(font, messages[i], group && !history_join(messages, i)) -
        (history_join(messages, i+1) ? 4 : 0);
}
static void draw_chat_bubble(float x, float y, float w, float h, bool outgoing, bool before, bool after,
                             unsigned fill, bool focused, unsigned accent) {
    const auto shape = chat_bubble::shape(x, y, w, h, outgoing, before, after);
    auto* v = radial::vertices(shape.count+2);
    if (!v) return;
    v[0] = {shape.center.x, shape.center.y, 0.5f, fill};
    for (int i = 0; i < shape.count; ++i)
        v[i+1] = {shape.points[i].x, shape.points[i].y, 0.5f, fill};
    v[shape.count+1] = v[1];
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, shape.count+2);
    if (focused) {
        auto* ring = radial::vertices((shape.count+1)*2);
        if (!ring) return;
        for (int i = 0; i <= shape.count; ++i) {
            const int j = i%shape.count;
            const auto p = shape.points[j], n = chat_bubble::outward(shape, j);
            ring[2*i] = {p.x, p.y, 0.5f, accent};
            ring[2*i+1] = {p.x+2*n.x, p.y+2*n.y, 0.5f, accent};
        }
        vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, ring, (shape.count+1)*2);
    }
}
// Shared by drawing and touch hit testing; date pills are not messages.
static std::vector<HistoryRow> history_layout(vita2d_pgf* font, TelegramClient& tg, int selected, int finger_delta = 0) {
    const auto& messages = tg.messages();
    const bool group = tg.currentChatIsGroup();
    static int64_t chat = 0, thread = 0;
    static int topic = 0, width = 0, pitch = 0;
    if (chat != tg.currentChatId() || thread != tg.currentThreadId() || topic != tg.currentTopicId() ||
        width != history_view.bodyWidth() || pitch != history_pitch() || selected < 0) {
        history_scroll = {};
        chat = tg.currentChatId(); thread = tg.currentThreadId(); topic = tg.currentTopicId();
        width = history_view.bodyWidth(); pitch = history_pitch();
    }
    auto id = [&](int i) { return messages[i].id; };
    auto separator = [&](int i) { return i == 0 || message_day(messages[i]) != message_day(messages[i-1]); };
    auto inset = [&](int i) { return separator(i) ? 26 : 0; };
    auto height = [&](int i) { return history_message_height(font, messages, i, group) + (separator(i) ? 26 : 0); };
    history_scroll.configure((int)messages.size(), history_bottom-history_view.top, selected, id, height, inset);
    if (finger_delta) history_scroll.drag(finger_delta, id, height, inset);
    std::vector<HistoryRow> rows;
    int y = history_view.top-history_scroll.offset();
    for (int i = history_scroll.first(); i < (int)messages.size() && y < history_bottom; ++i) {
        const bool day = separator(i);
        const int top = y+(day ? 26 : 0), body = history_message_height(font, messages, i, group);
        rows.push_back({i, top, body, day ? y : -1, message_day(messages[i]),
            history_join(messages, i), history_join(messages, i+1)});
        y = top+body;
    }
    return rows;
}

struct HistoryDateChip {
    int x = 0, y = 0, width = 0, index = -1;
    bool hit(int px, int py) const { return index >= 0 && px >= x && px < x+width && py >= y && py < y+22; }
};
static HistoryDateChip history_date_chip(vita2d_pgf* font, const std::vector<HistoryRow>& rows) {
    for (const auto& row : rows) if (row.top+row.height > history_view.top) {
        const int width = rich_text_width(font, 0.55f, row.day.c_str())+24;
        return {history_view.center()-width/2, std::max(history_view.top, row.day_top), width, row.index};
    }
    return {};
}
static ChatCalendar local_calendar(int64_t timestamp) {
    SceDateTime date{}; SceRtcTick utc{}, local{};
    if (sceRtcSetTime_t(&date, static_cast<time_t>(timestamp)) >= 0 && sceRtcGetTick(&date, &utc) >= 0 &&
        sceRtcConvertUtcToLocalTime(&utc, &local) >= 0) sceRtcSetTick(&date, &local);
    return {date.year, date.month, date.day};
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
    unsigned int color = ui_skin::color("delivery", RGBA8(65,133,72,255));
    if (state == DeliveryState::Failed) {
        color = ui_skin::color("danger", RGBA8(204,55,55,255));
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
    if (message.document) {
        result.push_back({tr("hint.download"), ""});
        if (message.document_size > 0)
            result.push_back({tr("media.file") + " · " + media_byte_count(message.document_size), ""});
    }
    PostLine reaction_line("", "");
    int reaction_width = 0;
    for (const auto& reaction : message.reactions) {
        std::string label = (reaction.custom_emoji_id ? "" : reaction.emoji + " ") + std::to_string(reaction.count);
        int width = rich_text_width(font, 0.68f, label.c_str()) + 14 + (reaction.custom_emoji_id ? 24 : 0);
        if (!reaction_line.reactions.empty() && reaction_width + width > history_view.width(840)) {
            result.push_back(std::move(reaction_line));
            reaction_line = PostLine("", ""); reaction_width = 0;
        }
        reaction_line.reactions.push_back({label, width, reaction.chosen, reaction.custom_emoji_id});
        reaction_width += width + 6;
    }
    if (!reaction_line.reactions.empty()) result.push_back(std::move(reaction_line));
    if (message.delivery == DeliveryState::Failed && !message.send_error.empty())
        for (const auto& line : wrap_text(font, message.send_error, history_view.width(840))) result.push_back({line, ""});
    // Transcription uses the same text panel as the message text (same style, padding and scrolling).
    if (!message.transcription.empty()) {
        PostLine header(tr(message.transcription_pending ? "post.transcription_pending" : "post.transcription"), "");
        header.transcript = true;
        result.push_back(std::move(header));
        for (const auto& line : wrap_text(font, message.transcription, history_view.width(840))) { PostLine entry(line, ""); entry.transcript = true; result.push_back(std::move(entry)); }
    }
    const auto wrapped = wrap_text(font, message.text, history_view.width(840) - (has_block_quote(message) ? 36 : 0));
    std::vector<PostLine> text_lines;
    const auto mapped = map_links_to_lines(message.text, wrapped, message.link_spans);
    const auto styles = map_styles(message, wrapped);
    for (size_t k = 0; k < wrapped.size(); ++k) {
        const std::string& line = wrapped[k];
        std::string target;
        int matches = 0;
        for (const auto& url : message.links) if (line.find(url) != std::string::npos) { target = url; ++matches; }
        PostLine entry(line, mapped[k].empty() ? (matches == 1 ? target : "") : mapped[k].front().url);
        entry.links = mapped[k];
        entry.styles = styles[k];
        text_lines.push_back(std::move(entry));
    }
    layout_block_quotes(font, message, wrapped, text_lines, post_pitch());
    for (auto& line : text_lines) result.push_back(std::move(line));
    // Links drawn inline are not repeated; the rest (e.g. hidden in rich messages) keep their own tappable rows.
    for (const auto& url : message.links) {
        const bool inline_link = std::any_of(message.link_spans.begin(), message.link_spans.end(), [&](const LinkSpan& span) { return span.url == url; });
        if (!inline_link) result.push_back({fit_text(font, tr("hint.open") + url, history_view.width(840), 0.68f), url});
    }
    if (message.has_comments) {   // last row: below the content and the reactions
        PostLine row(comments_label(message), "");
        row.comments = true;
        result.push_back(std::move(row));
    }
    return result;
}

struct PhotoTile { int index, x, width; };
static std::vector<PhotoTile> photo_carousel(const MessageItem& message, int selected) {
    std::vector<PhotoTile> tiles;
    int x = 0, offset = 0;
    for (int i=0; i<(int)message.photos.size(); ++i) {
        const auto& photo = message.photos[i];
        float aspect = photo.width > 0 && photo.height > 0 ? (float)photo.width/photo.height : 1.0f;
        int width = std::max(100, std::min(360, (int)(170 * aspect)));
        if (i == selected) offset = x + width/2 - history_view.center();
        tiles.push_back({i, x, width}); x += width+12;
    }
    offset = std::max(-history_view.x(24), std::min(offset, std::max(-history_view.x(24), x-936)));
    for (auto& tile : tiles) tile.x -= offset;
    return tiles;
}
// Post geometry follows the message viewport; media viewers keep the full screen.
static constexpr int kPostShift = 12;
static int post_photo_top() { return history_view.y(80 + kPostShift); }
static int post_media_top() { return history_view.y(80 + kPostShift); }
static constexpr int post_media_height = 340;   // tallest media area
// Height of the media area from the real aspect ratio, so wide clips don't leave an empty band below.
static int post_media_height_for(const MessageItem& message) {
    if (message.video_width > 0 && message.video_height > 0 && !message.video_note) {
        const float fitted = message.video_height * std::min((float)history_view.width(900) / message.video_width, (float)post_media_height / message.video_height);
        return std::max(120, std::min(post_media_height, (int)std::lround(fitted)));
    }
    return post_media_height;
}
// The text lives in a framed panel below the media; this is its top edge. Media never sits under the panel.
static int post_panel_top(const MessageItem& message) {
    if (message.voice) return history_view.y(318);
    if (!message.photos.empty()) return history_view.y(296 + kPostShift);
    if (message.animation || message.sticker)
        return post_media_top() + post_media_height_for(message) + (message.video && !message.video_note ? 70 : message.video ? 36 + scaled_px(14) : 30) +
            (message.video && message.animation_file_id > 0 && message.animation_error.empty() && video_load_view(message).downloading ? 72 : 0);
    return history_view.y(96 + kPostShift);
}
// Height the reply quote adds at the top of the text panel (quote + 8 px gap).
static int post_quote_extra(const MessageItem& message) {
    if (!message.reply_to_id && !message.forwarded) return 0;
    return 8 + (message.forwarded ? forward_height() : 0) + (message.reply_to_id ? quote_height() : 0);
}
static int post_text_top(const MessageItem& message) { return post_panel_top(message) + post_quote_extra(message) + 10 + scaled_px(18); }
static constexpr int kPanelBottomPad = 14;
// The text panel exists only when the message has text (or a transcription); reactions or hints alone stay frameless.
static bool post_has_text(const MessageItem& message) {
    return message.text.find_first_not_of(" \t\r\n") != std::string::npos || !message.transcription.empty() || message.reply_to_id != 0 || message.forwarded;
}
// Row position while the transcription block unfolds: its rows slide in together, the others make room.
static float reveal_row(int index, int transcript_rows, float reveal) {
    return index < transcript_rows ? index * reveal : transcript_rows * reveal + (index - transcript_rows);
}
static int count_transcript_rows(const std::vector<PostLine>& lines) {
    int rows = 0;
    for (const auto& line : lines) if (line.transcript) ++rows; else break;
    return rows;
}
static void draw_post_date(vita2d_pgf* font, const MessageItem& message, int scroll,
                           unsigned int background, unsigned int color) {
    std::string label = message_day(message) + "  " + message_time(message);
    if (history_view.combined) label = fit_text(font, label, history_view.width(960)-268, 0.55f);
    int width = rich_text_width(font, 0.55f, label.c_str()) + 28;
    const int left = history_view.combined ? history_view.x(32) : history_view.center()-width/2;
    skin_box(ui_skin::Slice::DateChip, left, history_view.top-scroll, width, 22, background, true);
    draw_text(font, left+14, history_view.top+16-scroll, label, color, 0.5f);
}
static int post_scroll_max(const MessageItem& message, size_t lines) {
    return std::max(0, post_text_top(message) + std::max(0, (int)lines - 1) * post_pitch() + kPanelBottomPad + 10 + link_preview_height(message) - 544);
}
static int post_preview_top(const MessageItem& message, size_t lines) {
    return post_text_top(message) + std::max(0, (int)lines - 1) * post_pitch() + kPanelBottomPad;
}

static i18n::Text open_web_link(const std::string& url) {
    std::string lower = url.substr(0, 8);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if ((lower.compare(0, 7, "http://") && lower != "https://") || url.find_first_of("\r\n\t ") != std::string::npos)
        return i18n::text("link.invalid");
    int rc = sceAppMgrLaunchAppByUri(0x20000, ("webmodal: " + url).c_str());
    if (rc >= 0) return "";
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08X", (unsigned)rc);
    return i18n::text("link.browser_error", {code});
}

// Korean and Chinese need their own glyphs; other languages use the standard font chain.
static const char* language_font_path_for(const std::string& language) {
    return language == "ko" ? "app0:assets/NotoSansKR-Regular.otf" :
        language == "zh-Hans" ? "app0:assets/NotoSansSC-Regular.otf" :
        language == "zh-Hant" ? "app0:assets/NotoSansTC-Regular.otf" : nullptr;
}
static void apply_language_font() {
    rich_text_set_language_font(language_font_path_for(i18n::current()));
}

static void startup_screen(vita2d_pgf* font, const char* stage, int step) {
    startup_trace::write(stage);
    vita2d_set_clear_color(RGBA8(16,24,33,255));
    vita2d_start_drawing();
    vita2d_clear_screen();
    if (font) {
        // System font only: no dependency on the large Noto/emoji assets.
        vita2d_pgf_draw_text(font, 330, 225, RGBA8(35,135,210,255), 1.5f, "VitaGram " VITAGRAM_VERSION);
        vita2d_pgf_draw_text(font, 280, 285, RGBA8(231,238,245,255), 0.8f, "Starting application...");
        vita2d_pgf_draw_text(font, 280, 325, RGBA8(163,183,200,255), 0.7f, stage);
    }
    vita2d_draw_rectangle(240, 370, 480, 6, RGBA8(53,70,86,255));
    vita2d_draw_rectangle(240, 370, 480.0f*step/5, 6, RGBA8(35,135,210,255));
    vita2d_end_drawing();
    vita2d_swap_buffers();
    vita2d_wait_rendering_done();
    sceGxmDisplayQueueFinish();
}

static std::string theme_name(int id) {
    if (const auto* custom = ui_themes::custom(id)) return custom->filename.substr(0, custom->filename.size() - 5);
    const auto& preset = ui_themes::get(id);
    return std::string(preset.brand) + (*preset.brand ? " / " : "") +
        tr(preset.base == 2 ? "settings.theme_vhbb" : preset.base == 1 ? "settings.theme_dark" : "settings.theme_light");
}

static void theme_switch_screen(vita2d_pgf* font) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    ui_skin::background();
    skin_box(ui_skin::Slice::Dialog, 150, 212, 660, 120,
             ui_skin::surface_color(RGBA8(31,45,59,255)), true);
    const std::string message = tr("settings.changing_theme");
    const unsigned ink = ui_skin::color("text", RGBA8(231,238,245,255));
    draw_text(font, 480 - rich_text_width(font, 0.85f, message.c_str()) / 2, 280, message, ink, 0.85f);
    vita2d_end_drawing();
    vita2d_swap_buffers();
    vita2d_wait_rendering_done(); // Keep the splash visible before textures are recolored or freed.
}

// Settings are a scrolling list laid out like the chat list: 47 px rows from y = 66.
constexpr int kSettingsTop = ui_layout::settings_top,
    kSettingsRowHeight = ui_layout::settings_row, kSettingsViewport = ui_layout::settings_viewport;

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

// Staged updates live in the app data folder; the Downloads folder is the fallback (see Updater).
static const char kUpdateDir[] = "ux0:data/vitagram/update";
static const char kUpdateFallbackDir[] = "ux0:downloads/vitagram-update";

// Shown while a staged update is installed: the install blocks for a few seconds and the app then
// restarts, which otherwise looks like a black screen. Only the system font is available at this point,
// and it has no CJK glyphs, so those languages get the English text.
struct UpdateText { std::string title, hint; };
static UpdateText update_screen_text() {
    const std::string language = i18n::current();
    const bool system_font_covers = language != "ja" && language != "ko" && language != "zh-Hans" && language != "zh-Hant";
    return system_font_covers ? UpdateText{tr("update.applying"), tr("update.applying_hint")}
                              : UpdateText{"Installing update...", "Please wait, do not turn off the console. VitaGram will restart."};
}
static void update_screen(vita2d_pgf* font, int percent) {
    const UpdateText text = update_screen_text();
    const std::string& title = text.title;
    const std::string& hint = text.hint;
    const std::string name = "VitaGram " VITAGRAM_VERSION;
    for (int frame = 0; frame < (percent <= 0 ? 2 : 1); ++frame) { // both buffers on the first call, so the screen stays up
        vita2d_set_clear_color(RGBA8(16,24,33,255));
        vita2d_start_drawing();
        vita2d_clear_screen();
        if (font) {
            auto centered = [&](int y, unsigned int color, float scale, const std::string& line) {
                vita2d_pgf_draw_text(font, 480 - vita2d_pgf_text_width(font, scale, line.c_str()) / 2, y, color, scale, line.c_str());
            };
            centered(225, RGBA8(35,135,210,255), 1.5f, name);
            centered(285, RGBA8(231,238,245,255), 0.9f, title);
            centered(325, RGBA8(163,183,200,255), 0.65f, hint);
        }
        vita2d_draw_rectangle(240, 380, 480, 8, RGBA8(53,70,86,255));
        vita2d_draw_rectangle(240, 380, 480.0f * std::min(std::max(percent, 0), 100) / 100, 8, RGBA8(35,135,210,255));
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
    vita2d_wait_rendering_done();
}

// newlib reserves a fixed heap at startup (default 128 MB). TDLib ran out of it during a large
// download (uncaught bad_alloc) while ~60 MB of user memory outside the heap stayed unused.
extern "C" unsigned int _newlib_heap_size_user = 160 * 1024 * 1024;

int main() {
    pthread_init();
#if !VITAGRAM_PICKER_LOG
    std::remove("ux0:data/vitagram/picker.log"); // file names from the card, written by older versions
#endif
    std::string update_failure; // result of installing a staged update, shown in Settings
    const int64_t startup_time = performance::now();
    performance::collector().start(startup_time);
    performance::startup_origin().store(startup_time);
    // Startup is CPU-bound (fonts, caches, the TDLib update flood): the ARM core runs at 444 MHz
    // until the first live chat list, at most 45 s, then returns to its clock. A faster one is kept.
    const int startup_clock_base = scePowerGetArmClockFrequency();
    bool startup_clock_raised = startup_clock_base < 444 && scePowerSetArmClockFrequency(444) >= 0;
    bool startup_window_closed = false;
    const int64_t startup_clock_until = sceKernelGetSystemTimeWide() + 45000000;
    cpu_scheduler::pinParser(true); // Same window as the clock: the startup update flood.
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
    int skin_theme = -1;   // theme of the loaded ui_skin textures (-1: none yet)
    vault::load();
    // Freed heap blocks are zeroed while the vault is on (see memory_hygiene.h).
    memory_hygiene::set_enabled(vault::enabled());
    // With the vault on, settings.json is sealed: it is read after the key has been entered, and
    // the key screens use the console language until then.
    ensure_download_directory("ux0:data/vitagram/lang");
    i18n::init("app0:lang", "", "ux0:data/vitagram/lang");
    keyboard_layout::all(); // Register validated custom profiles before reading saved preferences.
    if (!vault::enabled()) { settings.load(); diag_log::set_mask(settings.log_mask); }
    else settings.theme = AppSettings::themeHint(settings.theme);   // unlock screens follow the saved theme
    startup_trace::write("VitaGram " VITAGRAM_VERSION, true);
    i18n::select(settings.language);
    apply_language_font();

    vita2d_init();
    vita2d_set_vblank_wait(1);
    vita2d_set_clear_color(RGBA8(16, 24, 33, 255));
    vita2d_pgf* font = vita2d_load_default_pgf();
    // A staged update is installed now, while almost no memory is in use (see Updater::applyPending):
    // only the system font and the language file are loaded, none of the large fonts or TDLib.
    if (Updater::hasPending(kUpdateDir, kUpdateFallbackDir)) {
        update_screen(font, 0);
        int shown = 0;
        const Updater::Progress show = [&](int percent) {
            if (percent == shown) return; // redraw only when the bar moves
            shown = percent;
            update_screen(font, std::max(percent, 1));
        };
        switch (Updater::applyPending(kUpdateDir, kUpdateFallbackDir, VITAGRAM_VERSION, Updater::HandoffText{update_screen_text().title, update_screen_text().hint}, show, update_failure)) {
            // The VitaGram Updater app closes this process's leftovers, installs the package and starts VitaGram again.
            case Updater::Pending::Handoff: sceKernelExitProcess(0); break;
            case Updater::Pending::Failed: break; // reported in Settings below
            case Updater::Pending::None: break;
        }
    }
    // A successful update leaves the updater app behind; remove it now (nothing is pending any more).
    if (Updater::updaterCleanupDue(kUpdateDir, kUpdateFallbackDir)) {
        startup_screen(font, "Finishing the update...", 0);
        Updater::removeUpdaterApp(kUpdateDir, kUpdateFallbackDir);
    }
    startup_screen(font, "1/5  Loading fonts and emoji", 1);
    performance::Scope font_measurement(performance::Metric::FontsInit);
    if (!rich_text_init()) std::fprintf(stderr, "Noto text/emoji font initialization incomplete\n");
    font_measurement.finish();
    startup_trace::write("Fonts initialized; entering encryption gate");
    // The proxy policy could not be confirmed (settings expected but unreadable, or unsigned vault
    // state): TDLib is not started until the user saves the proxy settings - with or without a proxy.
    bool network_held = false;
    // Encryption gate: first-run question, key entry, and moving old files into/out of the vault.
    // It runs before the network and TDLib start so no file is changed while TDLib may use it.
    {
        const vault_ui::Env vault_env{font, settings.isDark(), settings.theme, settings.scroll_speed, settings.keyboard_languages, settings.keyboard_split};
        vault_ui::run_gate(vault_env);
        memory_hygiene::set_enabled(vault::enabled()); // the vault may have been created just now
        if (vault::state_unauthenticated()) {
            // vault.json carried no valid signature (edited, stripped, or from an older version - these
            // cannot be told apart): the caches may have been replaced as well. They are only caches -
            // TDLib rebuilds them - so they are dropped rather than sealed and trusted.
            for (const char* cache : {VITAGRAM_DB_DIR "/ui-cache.json", VITAGRAM_DB_DIR "/ui-startup.json", VITAGRAM_DB_DIR "/media-cache.json"})
                std::remove(cache);
            UiCache::removeHistoryFiles();
        }
        if (vault::enabled()) {
            settings.load();
            diag_log::set_mask(settings.log_mask);
            // Crash dumps hold decrypted memory in plain form outside the vault. They are kept only
            // when the user collects diagnostics.
            if (!settings.log_mask) {
                const int removed = crash_dump::purge("ux0:data", process_modules::kTitleId);
                if (removed) std::fprintf(stderr, "Vault: removed %d crash dump(s)\n", removed);
            }
            i18n::select(settings.language);
            apply_language_font();
            network_held = proxy_policy_unknown(settings);
            if (network_held) vault_ui::show_notice(vault_env, tr("network.held_notice"));
        }
        vault_ui::run_migration(vault_ui::Env{font, settings.isDark(), settings.theme, settings.scroll_speed, settings.keyboard_languages, settings.keyboard_split});
    }
    user_fonts::scan(); // Create the user directory even before a font has been selected.
    if (!rich_text_set_custom_font(settings.font_file)) settings.font_file.clear();
    startup_trace::write("Loading interface theme");
    ui_skin::load(settings.theme);
    startup_trace::write("Loading battery icons");
    ui_battery::load(settings.theme);
    startup_trace::write("Updating font icons");
    rich_text_skin_changed();
    skin_theme = settings.theme;
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
    tg.setVideoQuality(settings.video_quality);
    tg.setChatOpenPosition(settings.chat_open_position);
    tg.setShowArchive(settings.show_archive);
    startup_screen(font, "3/5  Starting Telegram transport", 3);
    if (net_ok && !network_held) tg.start(true);

    // The main loop can process authorization, navigation and live updates while
    // the storage worker prepares a fallback snapshot. Fresh Telegram data wins.
    startup_screen(font, "4/5  Starting cache restore", 4);
    tg.restoreCacheAsync();

    Keyboard kb;
    startup_screen(font, "5/5  Loading settings", 5);
    apply_font_scale(settings.fontScale());
    bool settings_open = false, confirm_logout = false, confirm_vault_off = false;
    int settings_row = 0;
    ThemePicker theme_picker;
    bool font_picker = false, font_pending = false, language_picker = false;
    std::vector<std::string> font_files;
    std::string requested_font;
    const auto font_name = [&](int index) { return index == 0 ? tr("settings.font_default") : font_files[index - 1]; };
    Updater updater(VITAGRAM_VERSION, "app0:certs/cacert.pem", kUpdateDir, kUpdateFallbackDir);
    if (!update_failure.empty()) updater.reportStartupFailure(update_failure);
    else for (const char* dir : {kUpdateDir, kUpdateFallbackDir}) {
        // from a failed install at an earlier start (written by VitaGram or by the updater app)
        FILE* f = std::fopen((std::string(dir) + "/result.txt").c_str(), "rb");
        if (!f) continue;
        char text[160]; size_t n = std::fread(text, 1, sizeof(text) - 1, f); std::fclose(f); text[n] = 0;
        if (n) { updater.reportStartupFailure(text); break; }
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
            case Updater::Error::Helper: return tr("update.err.helper");
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
    int keyboard_settings = 0; // 0 root, 1 keyboard, 2 input languages.
    bool cache_cleanup_requested = false;
    int settings_section = 0;
    bool logs_open = false;   // diagnostic-log switches, a sub-screen of the settings
    bool about_open = false;
    int about_offset = 0;
    int about_link = -1;
    bool about_chat_pending = false;
    int proxy_input = -1, proxy_scan_row = 0;
    DownloadFolderPicker folder_picker;
    UploadPicker upload_picker;
    bool upload_text_input = false;
    int64_t attach_menu_since = 0;
    // Seeking by touch: which control the finger started on (1 voice waveform, 2 video bar, 3 note ring, 4 play button).
    int scrub_kind = 0;
    float scrub_fraction = 0;
    // Voice notes playable straight from the chat list: the message whose source is loaded, play buttons drawn last frame.
    int64_t history_voice_id = 0;
    bool history_voice_toggle = false;
    std::vector<std::pair<int64_t, Rect4>> voice_play_rects;
    std::vector<std::pair<int64_t, Rect4>> comments_rects;   // comment badges of the visible channel posts (touch)
    std::vector<std::pair<int64_t, Rect4>> reply_rects;   // reply quotes of the visible messages
    Rect4 scrub_voice, scrub_bar, scrub_play, scrub_ring, reply_hit, delete_hit;   // reply_hit: the quote panel of the open post   // published by the last drawn frame (ring: x,y = centre, w = radius)
    int64_t delete_hit_message = 0, delete_touch_message = 0, delete_touch_chat = 0;
    float media_ui_alpha = 1.f;    // fullscreen photo/video controls fade in and out over 100 ms
    int64_t media_ui_stamp = 0;
    int64_t media_active_at = 0;   // last button/touch activity; fullscreen photo/video hide the corner button after 1 s
    bool chat_menu_open = false, chat_menu_waiting = false;
    std::vector<ChatMenuItem> chat_menu_snapshot;
    int64_t chat_menu_chat = 0, chat_menu_message = 0, chat_menu_wait_since = 0;

    // Chat search (opened from the chat-list menu or the search field) and the confirmation dialog of leave / clear / delete.
    bool search_open = false, search_input = false, confirm_leave = false, search_bar_focus = false;
    int search_index = 0, search_first = 0, confirm_age = 0;
    int confirm_action = 0;   // what confirm_leave asks: 0 leave group, 1 delete private chat, 2 clear its history
    constexpr int kSearchTop = ui_layout::search_top, kSearchRow = ui_layout::search_row, kSearchRows = ui_layout::search_rows;
    // A transcription appearing in the open post is revealed over ~180 ms (tracked per message).
    int64_t tr_track_id = 0, tr_anim_since = 0;
    bool tr_track_has = false;
    SettingsTextMarquee settings_label_marquee, settings_value_marquee;
    int chat_menu_sel = 0;
    auto menu_first = [](int count, int selected) { const int rows = history_menu_rows(); return std::clamp(selected - (rows - 1), 0, std::max(0, count - rows)); };
    int64_t chat_menu_since = 0;
    bool folder_name_input = false;
    AppSettings proxy_draft = settings;
    if (network_held) { settings_open = proxy_open = true; } // straight to the proxy screen
    QrScanner qr_scanner;
    std::string auth_qr_text;
    QrCode auth_qr;
    vita2d_texture* auth_qr_texture = nullptr;
    bool proxy_return_to_screen = false, proxy_imported = false;
    std::string pending_proxy_link;
    std::string bot_alert;
    int bot_panel = 0, bot_focus = 0;
    int64_t bot_sheet_since = 0;
    bool bot_sheet_closing = false;
#if VITAGRAM_WEBKIT
    WebAppView webapp_view;
    WebKitDownload webkit_download;
    Keyboard webapp_keyboard;
    std::string webapp_error, webapp_theme_json, webapp_main_text;
    bool webapp_back = false, webapp_main = false, webapp_main_active = false, webapp_confirmed = false;
    float webapp_x = 480, webapp_y = 196;
    int64_t webapp_pointer_since = 0;
    WebAppGesture webapp_gesture;
    bool webapp_fullscreen = false, webapp_pointer_visible = true;
    unsigned webapp_header_fill = 0, webapp_bottom_fill = 0, webapp_background_fill = 0;
    int64_t webapp_input_deadline = 0;
    auto reset_webapp_view = [&] {
        webkit_download.cancel();
        webapp_view.close(); webapp_keyboard.close(); webapp_confirmed = false;
        webapp_error.clear(); webapp_theme_json.clear(); webapp_main_text.clear();
        webapp_main = webapp_back = false;
        webapp_header_fill = webapp_bottom_fill = webapp_background_fill = 0;
        webapp_input_deadline = 0;
        webapp_pointer_since = 0;
        webapp_gesture.reset(); webapp_fullscreen = false; webapp_pointer_visible = true;
    };
#endif
    int64_t bot_panel_chat = 0, bot_panel_topic = 0, bot_panel_message = 0;
    bot_buttons::Markup bot_snapshot;
    SettingsTextMarquee bot_marquee;
    struct BotMessageHit { int64_t message; int x, y, w, h, row, column; };
    std::vector<BotMessageHit> bot_message_hits;
    std::string pending_web_link;
    bool web_link_proceed = false;
    int web_link_scroll = 0;
    auto open_link = [&](const std::string& url) -> i18n::Text {
        if (url.compare(0, 20, "vitagram:transcribe/") == 0) {
            tg.requestTranscription(std::strtoll(url.c_str() + 20, nullptr, 10));
            return "";
        }
        if (is_proxy_link(url)) { pending_proxy_link = url; return ""; }
        if (tg.openTelegramLink(url)) {
#if VITAGRAM_WEBKIT
            reset_webapp_view();
#endif
            return "";
        }
        std::string lower = url;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        size_t host_start = lower.compare(0, 8, "https://") == 0 ? 8 : lower.compare(0, 7, "http://") == 0 ? 7 : 0;
        if (!host_start || url.find_first_of("\r\n\t ") != std::string::npos) return i18n::text("link.invalid");
        auto host_end = lower.find_first_of("/?#", host_start);
        std::string host = lower.substr(host_start, host_end == std::string::npos ? host_end : host_end-host_start);
        if (host == "t.me" || host == "telegram.me" || host == "telegram.dog" || host == "telegram.org")
            return open_web_link(url);
        pending_web_link = url;
        web_link_proceed = false; web_link_scroll = 0;
        return "";
    };
    i18n::Text settings_error;
    std::string requested_language = settings.language;
    bool language_pending = false;
    auto language_name = [](const std::string& code) {
        for (const auto& language : i18n::languages()) if (language.code == code) return language.name;
        return code;
    };
    auto language_choice = [](int index) { return index == 0 ? std::string() : i18n::languages()[index - 1].code; };
    auto language_choice_name = [&](int index) {
        return index == 0 ? tr("settings.language_auto", {language_name(i18n::resolve(""))}) : language_name(language_choice(index));
    };
    auto language_label = [&]() {
        return language_pending ? tr("settings.language_loading") :
            settings.language.empty() ? tr("settings.language_auto", {language_name(i18n::current())}) : language_name(settings.language);
    };
    int64_t next_cache_cleanup = sceKernelGetSystemTimeWide() + 300000000;
    TextureCache textures;
    AnimationPlayer animation;
    AnimationPlayer video_note_animation;
    VoicePlayer voice;
    AudioRecordingDialog<AudioRecorder, VoicePlayer> recorder;
    using AudioRecordingState = decltype(recorder)::State;
    i18n::Text audio_error;
    int64_t audio_chat = 0;
    int32_t audio_topic = 0;
    VideoNoteRecorder circle_recorder;
    std::unique_ptr<AnimationPlayer> circle_playback;
    // Playback that outlives the chat screen: a panel at the bottom left of the history pane controls it and a video note also
    // floats as a draggable circle. Its file keeps downloading after the chat is left (tg.setBackgroundMedia).
    struct BackgroundMedia {
        bool live = false, circle = false;
        int32_t file = 0;
        int64_t message = 0;
        int duration = 0;
        i18n::Text title;
        download_stream::Shared stream;
        float x = 880, y = 420;   // circle centre
    } bg_media;
    bool bg_autoplay = false, prev_anim_playing = false, bg_finished_handled = false;
    bool bg_show = false, bg_touch = false, bg_touch_bar = false, bg_touch_moved = false, bg_prev_touch = false;
    float bg_touch_dx = 0, bg_touch_dy = 0, bg_last_x = 0, bg_last_y = 0, bg_start_x = 0, bg_start_y = 0;
    constexpr float kBgRadius = 44;
    audio_bar::Layout bg_bar;
    audio_bar::Shoulders bg_shoulders;
    audio_bar::Control bg_control = audio_bar::None, bg_focus_control = audio_bar::Play;
    bool bg_focus = false, bg_volume_open = false;
    int bg_volume = 100, bg_unmuted_volume = 100;
    int64_t bg_seek_target = 0;
    int32_t bg_touch_file = 0;
    int64_t circle_chat = 0;
    int32_t circle_topic = 0;
    i18n::Text circle_error;
    int chat_index = 0;
    bool combined_list_focus = true;
    auto keyboard_rows = [&] { return keyboard_settings == 2 ? 1 + (int)keyboard_layout::all().size() : 3; };
    auto input_language_summary = [&] {
        std::string value;
        for (const auto& code : keyboard_languages::resolve(settings.keyboard_languages, i18n::systemLanguage())) {
            if (!value.empty()) value += " + ";
            value += keyboard_layout::get(code).badge;
        }
        return value;
    };
    auto input_language_name = [&](const keyboard_layout::Layout& profile) {
        return !profile.system() ? profile.name : tr(profile.code == "ja" ? "keyboard.lang_ja" : profile.code == "ko" ?
            "keyboard.lang_ko" : profile.code == "zh-Hans" ? "keyboard.lang_zh_hans" : "keyboard.lang_zh_hant");
    };
    ChatListScroll chat_scroll, settings_scroll, settings_root_scroll, settings_parent_scroll;
    int settings_root_row = 0, settings_parent_row = 0;
    auto save_settings_parent = [&] {
        settings_parent_row = settings_row; settings_parent_scroll = settings_scroll;
    };
    auto restore_settings_parent = [&] {
        settings_row = settings_parent_row; settings_scroll = settings_parent_scroll;
        settings_error.clear();
    };
    ChatListMotion chat_motion;
    // The avatar each chat row last drew: kept while a new path for the same chat decodes.
    std::unordered_map<int64_t, std::string> chat_avatar_shown;
    bool chat_drag = false, history_drag = false;
    chat_view::Pane touch_pane = chat_view::Pane::None;
    int chat_touch_y = 0;
    int message_index = -1;
    int photo_index = 0;
    int post_text_scroll = 0;
    bool post_open = false;
    bool reactions_open = false;
    bool stickers_open = false;
    bool gifs_open = false, gif_input = false;
    int gif_index = 0, gif_next_page = -1;
    bool sticker_pack_menu = false;
    int sticker_index = 0, sticker_pack = 0;
    PickerPress sticker_press, gif_press;
    ReactionPress message_pack_press;
    int64_t pack_touch_message = 0;
    int sticker_preview_index = -1, gif_preview_index = -1;
    int topic_index = 0;
    radial::Menu compose_wheel;
    const radial::Geometry& compose_geometry = ui_geometry();
    radial::Labels compose_labels;
    bool compose_labels_ready = false, radial_flow = false;
    radial::Context compose_origin;
    int reaction_index = 0;
    ReactionPress reaction_press;
    int poll_index = 0;
    int64_t poll_message = 0, poll_chat = 0;
    std::vector<int> poll_choices;
    std::string poll_signature;
    bool photo_open = false;
    bool video_fullscreen = false, video_quality_menu = false;
    int64_t quality_resume_chat = 0, quality_resume_message = 0, quality_resume_time = 0;
    int32_t quality_resume_file = 0;
    bool quality_resume_playing = false;
    media_seek::TimeSelection seek_time;
    media_seek::HoldSeek fullscreen_seek;
    bool fullscreen_seek_was_playing = false;
    bool seek_open = false, seek_audio = false, seek_was_playing = false, seek_touching = false, seek_touch_release = false;
    int64_t seek_chat = 0, seek_message = 0;
    bool unsupported_video_prompt = false;
    // Photos with a spoiler stay blurred until the user confirms; the confirmed messages live for this session only.
    bool spoiler_prompt = false;
    int64_t spoiler_prompt_chat = 0, spoiler_prompt_message = 0;
    SpoilerVisibility spoiler_visibility;
    auto spoiler_locked = [&](const MessageItem& m) { return m.spoiler && !spoiler_visibility.revealed(tg.currentChatId(), m.id); };
    auto reveal_spoiler = [&](const MessageItem& m) {
        spoiler_visibility.reveal(tg.currentChatId(), m.id);
        for (const auto& photo : m.photos) { // Reload both previews and any previously blurred full-size photo.
            textures.forget(photo.thumbnail_path);
            textures.forget(photo.full_path);
        }
        textures.forget(m.media_path);
        textures.forget(m.media_fallback);
    };
    int64_t unsupported_video_chat = 0, unsupported_video_message = 0;
    // Set when the decoder finds a raster the hardware rejects although the message metadata looked supported.
    int64_t raster_mismatch_chat = 0, raster_mismatch_message = 0;
    bool portrait_preparation_requested = false;
    bool portrait_play_pending = false;
    int64_t portrait_play_chat = 0, portrait_play_message = 0;
    int64_t portrait_preparation_chat = 0, portrait_preparation_message = 0;
    PhotoView photo_view;
    bool running = true;
    ExitGesture exit_gesture;
    bool touch_down = false, touch_dragged = false, touch_blocked = false;
    int touch_x = 0, touch_y = 0, touch_start_x = 0, touch_start_y = 0, touch_step_y = 0;
    unsigned int decode_cooldown = 0;
    int64_t next_network_poll = 0;
    int64_t next_header_poll = 0;
    char header_clock[6] = "--:--";
    int battery_percent = -1;
    bool battery_charging = false;
    i18n::Text browser_error;
    TransientStatus browser_notice;
    int64_t status_chat = 0, status_topic = 0;
    int folder_scroll = 0, folder_touch_x = 0, shown_folder_index = -1;
    int32_t shown_folder_id = -1;
    bool folder_drag = false;
    chat_view::FolderDrawer folder_drawer;
    int drawer_draw_shift = 0;

    auto post_animation = [&]() -> AnimationPlayer& {
        return post_open && !upload_picker.active() && message_index >= 0 && message_index < (int)tg.messages().size() &&
            tg.messages()[message_index].video_note && (!bg_media.live || !bg_media.circle ||
            bg_media.file == tg.messages()[message_index].animation_file_id) ? video_note_animation : animation;
    };
    auto invalidate_spoiler_view = [&](bool changed, bool returning_to_app = false) {
        if (!changed && !returning_to_app) return;
        spoiler_prompt = false;
        if (changed || returning_to_app) {
            animation.setSource("", false, false); tg.setAnimationTarget(-1);
            video_note_animation.setSource("");
            if (bg_media.circle) bg_media.live = false;
            video_fullscreen = false; seek_open = false; scrub_kind = 0;
            scrub_play = {}; scrub_bar = {}; scrub_ring = {};
            portrait_play_pending = portrait_preparation_requested = false;
        }
        // Fullscreen navigation must request consent again rather than keeping a sharp photo open.
        const bool close_view = photo_open && message_index >= 0 && message_index < (int)tg.messages().size() && tg.messages()[message_index].spoiler;
        if (close_view) { photo_open = false; photo_view.begin(); touch_down = false; }
        // Cached GPU textures and pending decodes may still contain an unmasked image.
        if (changed || close_view) { vita2d_wait_rendering_done(); textures.clear(); }
    };
    auto observe_spoiler_chat = [&]() {
        invalidate_spoiler_view(spoiler_visibility.observe(
            {tg.currentChatId(), tg.currentTopicId(), tg.topicsOpen(), tg.inComments()}, settings.spoilerHideChat()));
    };
    auto open_audio = [&]() {
        if (!tg.ready() || !tg.currentChatId() || !tg.canPostCurrent()) { browser_error = i18n::text("rec.wait_connect"); return; }
        if (circle_recorder.active()) return;
        audio_chat = tg.currentChatId(); audio_topic = tg.currentTopicId(); audio_error.clear();
        animation.setSource(""); video_note_animation.setSource(""); voice.setSource(""); bg_media.live = false;
        recorder.open(std::string(VITAGRAM_FILES_DIR) + "/voice-message-" + std::to_string(sceKernelGetSystemTimeWide()) + ".ogg");
    };
    auto open_circle = [&]() {
        if (recorder.active()) {
            browser_error = i18n::text("rec.finish_first"); return;
        }
        if (!tg.ready() || !tg.currentChatId()) { browser_error = i18n::text("rec.wait_connect"); return; }
        circle_chat = tg.currentChatId(); circle_topic = tg.currentTopicId(); circle_error.clear();
        circle_playback.reset();
        animation.setSource(""); video_note_animation.setSource(""); voice.setSource(""); bg_media.live = false;
        circle_recorder.open(std::string(VITAGRAM_FILES_DIR) + "/video-note-" +
            std::to_string(sceKernelGetSystemTimeWide()) + ".mp4");
    };

    bool startup_ready_measured = false, startup_live_list_measured = false;
    performance::observe(performance::Metric::StartupUi, performance::now()-startup_time);
    startup_trace::write("Main interface initialized");
    webapp_diagnostics::record(webapp_diagnostics::Stage::ClientReady, 0); // Engine loads only on demand.
    // Actions of the current chat screen; each maps to the physical button that already does it.
    // Join / Leave for groups and channels (TDLib membership status), Clear / Delete for private chats.
    auto membership_items = [&](std::vector<ChatMenuItem>& v) {
        using M = TelegramClient::Membership;
        if (tg.currentChatIsPrivate()) {
            v.push_back({"menu.clear_chat", 0, 12, (int)ui_skin::Icon::ClearHistory});
            v.push_back({"menu.delete_chat", 0, 13, (int)ui_skin::Icon::Leave});
            return;
        }
        const M state = tg.currentMembership();
        if (state == M::NotMember) v.push_back({"menu.join", 0, 10, (int)ui_skin::Icon::Check});
        else if (state == M::Member) v.push_back({"menu.leave", 0, 11, (int)ui_skin::Icon::Leave});
    };
    auto chat_menu_items = [&]() {
        std::vector<ChatMenuItem> v;
        const auto& m = tg.messages();
        const bool has_message = message_index >= 0 && message_index < (int)m.size();
        if (video_quality_menu && has_message && m[message_index].video && !m[message_index].video_note) {
            const auto& message = m[message_index];
            for (size_t i = 0; i < message.video_variants.size(); ++i) {
                const auto& variant = message.video_variants[i];
                const bool chosen = variant.file == message.animation_file_id && variant.path == message.animation_path;
                v.push_back({"video.quality", 0, 50 + (int)i, chosen ? (int)ui_skin::Icon::Check : -1,
                    std::to_string(variant.width) + " × " + std::to_string(variant.height)});
            }
            if (v.empty()) v.push_back({"video.quality_unavailable", 0, 3});
            v.push_back({"menu.back", 0, 42});
            return v;
        }
        if (recorder.active()) {
            const auto state = recorder.state();
            if (state == AudioRecordingState::Preview) v = {{"menu.rec_record", SCE_CTRL_CROSS, 0}};
            else if (state == AudioRecordingState::Recording) v = {{"menu.rec_stop", SCE_CTRL_CROSS, 0}};
            else if (state == AudioRecordingState::Review) v = {{"menu.rec_send", SCE_CTRL_CROSS, 0}, {"menu.rec_preview", SCE_CTRL_SQUARE, 0}};
            else if (state == AudioRecordingState::Error) v = {{"menu.rec_retry", SCE_CTRL_CROSS, 0}};
            v.push_back({"menu.rec_cancel", SCE_CTRL_CIRCLE, 0});
        } else if (circle_recorder.active()) {
            using S = VideoNoteRecorder::State;
            const S state = circle_recorder.state();
            if (state == S::Preview) v = {{"menu.rec_record", SCE_CTRL_CROSS, 0}, {"menu.rec_camera", SCE_CTRL_TRIANGLE, 0}};
            else if (state == S::Recording) v = {{"menu.rec_stop", SCE_CTRL_CROSS, 0}, {"menu.rec_camera", SCE_CTRL_TRIANGLE, 0}};
            else if (state == S::Review) v = {{"menu.rec_send", SCE_CTRL_CROSS, 0}, {"menu.rec_preview", SCE_CTRL_SQUARE, 0}};
            else if (state == S::Error) v = {{"menu.rec_retry", SCE_CTRL_CROSS, 0}};
            v.push_back({"menu.rec_cancel", SCE_CTRL_CIRCLE, 0});
        } else if (!tg.currentChatId() && !tg.topicsOpen()) {
            if (search_open) v = {{"menu.search_open", SCE_CTRL_CROSS, 0}, {"menu.search_edit", SCE_CTRL_SQUARE, 0, (int)ui_skin::Icon::Search},
                                  {"menu.refresh", SCE_CTRL_TRIANGLE, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
            else v = {{"menu.open_chat", SCE_CTRL_CROSS, 0}, {"menu.search", SCE_CTRL_SQUARE, 0, (int)ui_skin::Icon::Search},
                      {"menu.settings", SCE_CTRL_START, 0}, {"menu.refresh", SCE_CTRL_TRIANGLE, 0}};
            if (!search_open && tg.folders().size() > 1) { v.push_back({"menu.prev_folder", SCE_CTRL_LTRIGGER, 0}); v.push_back({"menu.next_folder", SCE_CTRL_RTRIGGER, 0}); }
        } else if (tg.topicsOpen()) {
            v = {{"menu.open_topic", SCE_CTRL_CROSS, 0}, {"menu.load_topics", SCE_CTRL_TRIANGLE, 0}};
            membership_items(v);
            v.push_back({"menu.back", SCE_CTRL_CIRCLE, 0});
        } else if (gifs_open) {
            v = {{"gif.send", SCE_CTRL_CROSS, 0}, {"gif.search", SCE_CTRL_SQUARE, 0, (int)ui_skin::Icon::Search},
                 {"gif.saved", SCE_CTRL_TRIANGLE, 0}, {"gif.previous", SCE_CTRL_LTRIGGER, 0},
                 {"gif.next", SCE_CTRL_RTRIGGER, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
        } else if (stickers_open) {
            if (sticker_pack_menu) v = {{"menu.open_set", SCE_CTRL_CROSS, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
            else if (tg.stickerSetEmoji()) v = {{"menu.sets", SCE_CTRL_CIRCLE, 0}, {"menu.close_picker", SCE_CTRL_SQUARE, 0}};
            else v = {{"menu.send_sticker", SCE_CTRL_CROSS, 0}, {"menu.sets", SCE_CTRL_CIRCLE, 0}, {"menu.close_picker", SCE_CTRL_SQUARE, 0}};
        } else if (reactions_open) {
            v = {{"menu.react_toggle", SCE_CTRL_CROSS, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
        } else if (video_fullscreen) {
            v.push_back({"video.quality", 0, 41});
            if (post_animation().readyForPlayback()) v.push_back({"menu.play_pause", SCE_CTRL_CROSS, 0});
            if (post_animation().canSeek()) {
                v.push_back({"menu.seek_back", SCE_CTRL_LTRIGGER, 0}); v.push_back({"menu.seek_forward", SCE_CTRL_RTRIGGER, 0});
                if (has_message && m[message_index].video_duration > 0) v.push_back({"menu.seek_time", 0, 40});
            }
            v.push_back({"menu.normal_view", SCE_CTRL_TRIANGLE, 0}); v.push_back({"menu.back", SCE_CTRL_CIRCLE, 0});
        } else if (photo_open) {
            v = {{"menu.prev_photo_dpad", SCE_CTRL_LEFT, 0}, {"menu.next_photo_dpad", SCE_CTRL_RIGHT, 0},
                 {"menu.rotate_left", SCE_CTRL_LTRIGGER, 0}, {"menu.rotate_right", SCE_CTRL_RTRIGGER, 0},
                 {"menu.reset_zoom", SCE_CTRL_TRIANGLE, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
        } else if (post_open && has_message && m[message_index].poll.id) {
            v = {{"menu.prev_post", SCE_CTRL_LTRIGGER, 0}, {"menu.next_post", SCE_CTRL_RTRIGGER, 0},
                 {"menu.poll_choose", SCE_CTRL_CROSS, 0}, {"menu.poll_send", SCE_CTRL_TRIANGLE, 0},
                 {"menu.poll_retract", SCE_CTRL_SQUARE, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
        } else if (post_open && has_message) {
            const MessageItem& item = m[message_index];
            v = {{"menu.prev_post", SCE_CTRL_LTRIGGER, 0}, {"menu.next_post", SCE_CTRL_RTRIGGER, 0}};
            if (item.photos.size() > 1) { v.push_back({"menu.prev_photo_dpad", SCE_CTRL_LEFT, 0}); v.push_back({"menu.next_photo_dpad", SCE_CTRL_RIGHT, 0}); }
            if (item.animation && spoiler_locked(item)) v.push_back({"spoiler.title", SCE_CTRL_CROSS, 0});
            if (!item.photos.empty()) v.push_back({"menu.open_photo", SCE_CTRL_CROSS, 0});
            if (item.voice || (item.video && (post_animation().readyForPlayback() || post_animation().preparationRequired()))) v.push_back({"menu.play_pause", SCE_CTRL_CROSS, 0});
            if ((item.voice || (item.video && post_animation().canSeek())) && item.photos.empty()) {
                v.push_back({"menu.seek_back_dpad", SCE_CTRL_LEFT, 0}); v.push_back({"menu.seek_forward_dpad", SCE_CTRL_RIGHT, 0});
            }
            if ((item.voice && !item.voice_path.empty() && item.voice_error.empty() && item.voice_duration > 0) ||
                (item.video && post_animation().canSeek() && item.video_duration > 0)) v.push_back({"menu.seek_time", 0, 40});
            if (item.video && !item.video_note) {
                v.push_back({"video.quality", 0, 41});
                v.push_back({"menu.fullscreen", SCE_CTRL_TRIANGLE, 0});
            }
            // Only voice messages and round videos can be transcribed (triangle); 3 is the "requested" note.
            if ((item.voice && !item.audio) || item.video_note) {
                if (item.transcription_pending && item.transcription.empty()) v.push_back({"post.transcription_wait", 0, 3});
                else if (!item.transcription_complete && item.transcription.empty()) v.push_back({"post.transcribe", SCE_CTRL_TRIANGLE, 0});
            }
            if (item.document) v.push_back({"menu.download", SCE_CTRL_CROSS, 0});
            if (item.reply_to_id && (!item.reply_chat_id || item.reply_chat_id == tg.currentChatId())) v.push_back({"menu.go_reply", 0, 4});
            if (item.has_comments && !tg.inComments()) v.push_back({"menu.comments", SCE_CTRL_SELECT, 20});
            if (!item.service_message) {
                if (tg.canPostCurrent()) v.push_back({"menu.reply", 0, 30, (int)ui_skin::Icon::Reply});
                v.push_back({"menu.forward", 0, 31, (int)ui_skin::Icon::Forward});
            }
            v.push_back({"menu.back", SCE_CTRL_CIRCLE, 0});
        } else {
            if (has_message) v.push_back({"menu.open_post", SCE_CTRL_CROSS, 0});
            if (has_message && m[message_index].has_comments && !tg.inComments()) v.push_back({"menu.comments_chat", SCE_CTRL_RIGHT, 20});
            if (has_message && !m[message_index].service_message) {
                if (tg.canPostCurrent()) v.push_back({"menu.reply", 0, 30, (int)ui_skin::Icon::Reply});
                v.push_back({"menu.forward", 0, 31, (int)ui_skin::Icon::Forward});
            }
            if (tg.forwardPending() && tg.canPostCurrent()) v.push_back({"menu.paste_forward", 0, 32});
            if (tg.forwardPending()) v.push_back({"menu.cancel_forward", 0, 33});
            v.push_back({"menu.compose", SCE_CTRL_SQUARE, 0});
            v.push_back({"menu.attach", SCE_CTRL_SELECT, 0});
            membership_items(v);
            v.push_back({"menu.refresh", SCE_CTRL_TRIANGLE, 0});
            v.push_back({"menu.back", SCE_CTRL_CIRCLE, 0});
        }
        if (stickers_open && !sticker_pack_menu) {
            if (tg.stickerSetOpen() && tg.stickerSetCanInstall()) v.push_back({"sticker.add_set", 0, 36});
            if (sticker_index >= 0 && sticker_index < (int)tg.stickers().size() && tg.stickers()[sticker_index].set_id)
                v.push_back({"sticker.open_set", 0, 35});
        } else if (has_message && !recorder.active() && !circle_recorder.active() && !search_open && !reactions_open && !gifs_open &&
            (m[message_index].sticker_set_id || !m[message_index].custom_emoji_ids.empty()))
            v.push_back({"sticker.open_set", 0, 35});
        if (has_message && !photo_open && !video_fullscreen && !stickers_open && !gifs_open && !reactions_open &&
            !circle_recorder.active() && !recorder.active() && !tg.topicsOpen() && tg.canDeleteMessage(m[message_index].id, false))
            v.push_back({"menu.delete_message", 0, 34, (int)ui_skin::Icon::Trash});
        return v;
    };
    auto open_selected_set = [&]() {
        int64_t set = 0, emoji = 0;
        if (stickers_open && !sticker_pack_menu && sticker_index >= 0 && sticker_index < (int)tg.stickers().size())
            set = tg.stickers()[sticker_index].set_id;
        else if (message_index >= 0 && message_index < (int)tg.messages().size()) {
            const auto& message = tg.messages()[message_index];
            set = message.sticker_set_id;
            if (!message.custom_emoji_ids.empty()) emoji = message.custom_emoji_ids.front();
        }
        if (!set && !emoji) return;
        tg.openStickerSet(set, emoji);
        stickers_open = true; sticker_pack_menu = false; sticker_index = 0;
        sticker_preview_index = -1; gifs_open = reactions_open = false;
    };
    auto attach_menu_items = [&]() {
        std::vector<ChatMenuItem> items = {{"upload.media", 0, 0},
            {"upload.file", 0, 1, (int)ui_skin::Icon::File}, {settings.recording_shortcuts ? "upload.video_note_shortcut" : "upload.video_note", 0, 3},
            {settings.recording_shortcuts ? "upload.voice_message_shortcut" : "upload.voice_message", 0, 4}};
        if (tg.canPostCurrent()) {
            items.push_back({"gif.menu", 0, 5, (int)ui_skin::Icon::Search});
            items.push_back({"sticker.title", 0, 6});
        }
        const auto& messages = tg.messages();
        if (message_index >= 0 && message_index < (int)messages.size() && !messages[message_index].service_message)
            items.push_back({"reaction.title", 0, 7});
        return items;
    };
    constexpr int attach_menu_row = ui_layout::menu_row;
    // Opens the discussion thread of the selected channel post; the reply (or an error notice) arrives asynchronously.
    auto open_comments = [&]() {
        const auto& list = tg.messages();
        if (message_index < 0 || message_index >= (int)list.size() || !list[message_index].has_comments) return false;
        return tg.openComments(list[message_index].id, post_open);
    };
    // Relative seeking for controllers (and the START menu): only when the current player supports it.
    auto seek_by = [&](const MessageItem& m, int64_t delta_us) {
        if (m.voice) {
            if (m.voice_path.empty() || m.voice_duration <= 0) return;
            const int64_t limit = std::max<int64_t>(0, (int64_t)m.voice_duration * 1000000 - 300000);
            voice.seekTo(std::max<int64_t>(0, std::min<int64_t>(limit, (int64_t)(voice.positionSeconds() * 1e6) + delta_us)));
        } else if (m.video && m.video_duration > 0 && post_animation().canSeek()) {
            const int64_t limit = std::max<int64_t>(0, (int64_t)m.video_duration * 1000000 - 500000);
            post_animation().seekTo(std::max<int64_t>(0, std::min<int64_t>(limit, post_animation().positionUs() + delta_us)));
        }
    };
    auto seek_source_current = [&]() {
        const auto& messages = tg.messages();
        return tg.currentChatId() == seek_chat && message_index >= 0 && message_index < (int)messages.size() &&
            messages[message_index].id == seek_message && !settings_open && !photo_open && post_open;
    };
    auto finish_seek = [&](bool apply) {
        if (seek_source_current()) {
            if (seek_audio) {
                if (apply) voice.seekTo(seek_time.targetUs());
                voice.setPlaying(seek_was_playing);
            } else if (post_animation().canSeek()) {
                if (apply) post_animation().seekTo(seek_time.targetUs());
                if (post_animation().playing() != seek_was_playing) post_animation().toggle();
            }
        }
        seek_open = false; seek_touching = false;
    };
    auto open_seek = [&]() {
        const auto& messages = tg.messages();
        if (message_index < 0 || message_index >= (int)messages.size()) return;
        const auto& message = messages[message_index];
        const bool audio = message.voice && !message.voice_path.empty() && message.voice_error.empty() && message.voice_duration > 0;
        if (!audio && !(message.video && post_animation().canSeek() && message.video_duration > 0)) return;
        seek_chat = tg.currentChatId(); seek_message = message.id; seek_audio = audio;
        seek_was_playing = audio ? voice.playing() : post_animation().playing();
        seek_time.begin(audio ? (int64_t)(voice.positionSeconds() * 1000000) : post_animation().positionUs(),
            (int64_t)(audio ? message.voice_duration : message.video_duration) * 1000000, audio ? 300000 : 500000);
        if (audio) voice.setPlaying(false);
        else if (seek_was_playing) post_animation().toggle();
        seek_open = true; seek_touching = false; touch_down = false; scrub_kind = 0;
    };
    // Tools own input, but never change the layout of the chat underneath.
    bool calendar_open = false;
    ChatCalendar calendar, calendar_today;
    int64_t calendar_chat = 0, calendar_thread = 0;
    int calendar_topic = 0;
    auto tool_window_open = [&]() {
        return search_open || stickers_open || gifs_open || reactions_open || upload_picker.active() ||
            circle_recorder.active() || recorder.active();
    };
    auto chat_modal_open = [&]() {
        return calendar_open || tool_window_open() || kb.active() || qr_scanner.active() || seek_open || seek_touch_release ||
            unsupported_video_prompt || spoiler_prompt || !pending_web_link.empty() || bot_panel ||
            tg.botWebApp().phase != TelegramClient::BotWebApp::Closed || confirm_leave || compose_wheel.busy();
    };
    auto chat_ui_visible = [&]() {
        return (tg.currentChatId() != 0 || tg.topicsOpen() || tg.canBrowse()) &&
            !settings_open && !chat_modal_open();
    };
    auto tool_menu_visible = [&]() {
        return !settings_open && !kb.active() && !upload_picker.active() &&
            (circle_recorder.active() || recorder.active() || stickers_open || gifs_open || search_open) &&
            !seek_open && !spoiler_prompt && !unsupported_video_prompt && pending_web_link.empty() &&
            !bot_panel && tg.botWebApp().phase == TelegramClient::BotWebApp::Closed && !confirm_leave;
    };
    auto combined_visible = [&]() {
        return settings.combined_view && tg.canBrowse() && !settings_open && !photo_open && !video_fullscreen;
    };
    auto update_audio_bar = [&]() {
        bg_show = bg_media.live && tg.canBrowse() && !settings_open && !post_open && !photo_open && !video_fullscreen &&
            !kb.active() && !qr_scanner.active() && !circle_recorder.active() && !recorder.active() &&
            !stickers_open && !gifs_open && !reactions_open && !upload_picker.active() && !bot_panel &&
            tg.botWebApp().phase == TelegramClient::BotWebApp::Closed && !seek_open && !seek_touch_release &&
            !confirm_leave && !unsupported_video_prompt && !spoiler_prompt && pending_web_link.empty() && !compose_wheel.busy();
        const bool toolbar = !tg.botMenu().empty() || !tg.botKeyboard().buttons.empty() ||
            (message_index >= 0 && message_index < (int)tg.messages().size() && !tg.messages()[message_index].buttons.empty());
        bg_bar = {(float)history_view.left(), toolbar ? 494.f : 544.f};
        history_bottom = bot_panel == 2 ? 312 : toolbar && tg.ready() ? 494 : 544;
        if (bg_show) history_bottom = (int)bg_bar.top();
        if (bg_show && bg_media.circle) bg_media.y = std::min(bg_media.y,bg_bar.top()-kBgRadius-4);
        chat_corner_bottom = bg_show ? bg_bar.top() : 544.f;
        kMenuBottom = ui_layout::menu_bottom - (int)(544-chat_corner_bottom);
        if (!bg_show) { bg_focus = false; bg_volume_open = false; }
    };
    auto bg_duration = [&]() { return int64_t(bg_media.duration)*1000000; };
    auto bg_position = [&]() { return bg_media.circle ? video_note_animation.positionUs() : (int64_t)(voice.positionSeconds()*1000000); };
    auto bg_limit = [&]() {
        if (bg_media.circle) return video_note_animation.canSeek() ? video_note_animation.seekLimitUs() : int64_t(0);
        if (bg_media.stream && !bg_media.stream->ready()) return int64_t(0);
        return voice.seekLimitUs(bg_duration());
    };
    auto bg_seek = [&](int64_t target) {
        if (bg_duration() <= 0) return;
        target = std::clamp(target, int64_t(0), bg_limit());
        if (bg_media.circle) { if (video_note_animation.canSeek()) video_note_animation.seekTo(target); }
        else voice.seekTo(target);
    };
    auto bg_set_volume = [&](int volume) {
        bg_volume = std::clamp(volume, 0, 100);
        if (bg_volume) bg_unmuted_volume = bg_volume;
        voice.setVolume(bg_volume); animation.setVolume(bg_volume); video_note_animation.setVolume(bg_volume);
    };
    auto bg_switch_track = [&](int direction) {
        if (bg_media.circle || !tg.audioTrack()) return;
        if (direction < 0 && bg_position() >= 3000000) { bg_seek(0); return; }
        if (!tg.advanceAudioQueue(direction, false)) { if (direction < 0) bg_seek(0); return; }
        const auto* track = tg.audioTrack();
        voice.setSource(""); history_voice_id = 0;
        bg_media.file = track->file; bg_media.message = track->message; bg_media.duration = track->duration;
        bg_media.title = track->title; bg_autoplay = true;
        tg.serviceAudioQueue();
    };
    auto bg_action = [&](audio_bar::Control control) {
        using namespace audio_bar;
        switch (control) {
        case Previous: bg_switch_track(-1); break;
        case Next: bg_switch_track(1); break;
        case Play:
            if (bg_media.circle) video_note_animation.toggle();
            else if (bg_autoplay) bg_autoplay = false;
            else if (tg.audioTrack() && tg.audioTrack()->path.empty() &&
                (!tg.audioTrack()->stream || !tg.audioTrack()->stream->ready())) bg_autoplay = true;
            else voice.toggle();
            break;
        case Volume: bg_volume_open = !bg_volume_open; break;
        case Mute: bg_set_volume(bg_volume ? 0 : bg_unmuted_volume); break;
        case Shuffle: if (!bg_media.circle && tg.audioTrack()) tg.shuffleAudioQueue(); break;
        case Repeat: if (!bg_media.circle && tg.audioTrack()) tg.cycleAudioRepeat(); break;
        case Speed: if (!bg_media.circle) voice.setSpeed(nextSpeed(voice.speed())); break;
        case Close:
            if (bg_media.circle) video_note_animation.setSource(""); else voice.setSource("");
            tg.clearAudioQueue(); history_voice_id = 0;
            bg_media.live = false; bg_autoplay = bg_focus = bg_volume_open = false;
            bg_media.stream.reset();
            break;
        default: break;
        }
    };
    // Vault auto-lock. Wall-clock time keeps running while the console sleeps or the app sits behind the
    // LiveArea, so a jump between two frames means the user left; ON_RESUME also catches short sleeps.
    constexpr uint64_t kLeftAppUs = 10000000;
    auto wall_us = []() -> uint64_t { SceRtcTick tick{}; sceRtcGetCurrentTick(&tick); return tick.tick; };
    uint64_t lock_previous_frame = 0, lock_last_activity = wall_us();
    int64_t next_video_power_tick = 0;
    bool lock_pending = false; // left the app while the system keyboard was open: lock once it closes
    ScrollInput scroll_input;
    ScrollRepeat scroll_repeat;
    auto lock_app = [&]() {
        tg.closeBotWebApp();
#if VITAGRAM_WEBKIT
        reset_webapp_view();
#endif
        fullscreen_seek = media_seek::HoldSeek{};
        fullscreen_seek_was_playing = false;
        seek_open = false; seek_touching = false;
        // Covering the screen is enough: TDLib, downloads and conversions keep running underneath.
        if (animation.playing()) animation.toggle();
        if (video_note_animation.playing()) video_note_animation.toggle();
        if (circle_playback && circle_playback->playing()) circle_playback->toggle();
        if (voice.playing()) voice.toggle();
        recorder.pausePreview();
        vault_ui::relock(vault_ui::Env{font, settings.isDark(), settings.theme, settings.scroll_speed, settings.keyboard_languages, settings.keyboard_split},
                         [](void* client) { static_cast<decltype(&tg)>(client)->pump(false); cpu_scheduler::tick(); }, &tg);
        SceCtrlData after{}; sceCtrlPeekBufferPositive(0, &after, 1); old_buttons = after.buttons;
        scroll_input.sample(after.buttons & SCE_CTRL_UP, after.buttons & SCE_CTRL_DOWN, after.ly);
        scroll_repeat = ScrollRepeat{};
        vita2d_set_clear_color(settings.isDark() ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255));
        lock_previous_frame = lock_last_activity = wall_us();
    };
    // Vault screens opened from Settings own the display for as long as the user stays there; keep
    // TDLib's events flowing meanwhile, as the lock screen does.
    auto settings_vault_env = [&]() {
        vault_ui::Env env{font, settings.isDark(), settings.theme, settings.scroll_speed, settings.keyboard_languages, settings.keyboard_split};
        env.tick = [](void* client) { static_cast<decltype(&tg)>(client)->pump(false); cpu_scheduler::tick(); };
        env.tick_context = &tg;
        return env;
    };
    enum class ScrollContext {
        WebLink, ChatMenu, Topics, AttachMenu, UploadBrowse, UploadReview,
        Gifs, StickerPacks, Stickers, Reactions, Folders, About, Logs, Proxy,
        KeyboardSettings, Settings, ThemePicker, Poll, Post, History, Search, Chats, Bots, FolderDrawer
    };
    auto update_performance_context = [&](bool controller_scroll = false) {
        if (!performance::enabled) return;
        auto& context = performance::frame_context;
        using Screen = performance::Screen;
        context.screen = settings_open ? Screen::Settings :
            tg.botWebApp().phase != TelegramClient::BotWebApp::Closed ? Screen::WebApp :
            photo_open ? Screen::Photo : video_fullscreen ? Screen::Video :
            recorder.active() || circle_recorder.active() ? Screen::Recorder :
            upload_picker.active() ? Screen::Upload : stickers_open ? Screen::Stickers :
            gifs_open ? Screen::Gifs : reactions_open ? Screen::Reactions :
            tg.topicsOpen() ? Screen::Topics : post_open ? Screen::Post :
            search_open ? Screen::Search : tg.currentChatId() ? Screen::History :
            tg.canBrowse() ? Screen::Chats : Screen::Auth;
        context.combined = settings.combined_view;
        context.keyboard = kb.active();
        context.scrolling = chat_drag || history_drag || controller_scroll;
        context.fullscreen = photo_open || video_fullscreen;
        context.rotation = photo_open ? photo_view.rotation : 0;
        context.web_width = context.web_height = 0;
#if VITAGRAM_WEBKIT
        context.keyboard = context.keyboard || webapp_keyboard.active();
        if (webapp_view.active()) {
            context.fullscreen = webapp_view.fullscreen();
            context.rotation = webapp_view.rotation();
            context.scrolling = context.scrolling || webapp_gesture.dragged ||
                std::abs(webapp_gesture.vx) >= 5 || std::abs(webapp_gesture.vy) >= 5;
            const int height = context.fullscreen ? 544 : 392;
            context.web_width = context.rotation ? height : 960;
            context.web_height = context.rotation ? 960 : height;
        }
#endif
    };
    while (running) {
        if (performance::enabled) ++performance::frame_context.frame;
        update_performance_context();
        performance::Scope frame_measurement(performance::Metric::Frame);
        performance::Scope ui_measurement(performance::Metric::UiWork);
        performance::SlowScope slow_ui(performance::SlowPhase::Ui,"frame_ui");
        ui_diagnostics::Stage ui_stage(ui_diagnostics::Phase::Frame);
        performance::SlowScope pre_input(performance::SlowPhase::Ui,"pre_input");
        if (tg.hasSavedDocuments() && !kb.active() && !recorder.active() && !circle_recorder.active() &&
            updater.allowsLocalInstall() && !confirm_leave && !unsupported_video_prompt &&
            !spoiler_prompt && pending_web_link.empty() && !seek_open) {
            const std::string saved = tg.takeSavedDocument();
            if (!saved.empty() && settings.allow_vpk_install && vpk::filename(saved)) {
                voice.setPlaying(false);
                if (animation.playing()) animation.toggle();
                if (video_note_animation.playing()) video_note_animation.toggle();
                if (circle_playback && circle_playback->playing()) circle_playback->toggle();
                if (install_downloaded_vpk(saved)) {
                    tg.close(); running = false;
                    break; // The updater installs and launches VitaGram after this process exits.
                }
                sceCtrlPeekBufferPositive(0, &pad, 1); old_buttons = pad.buttons;
                seek_touch_release = true; touch_down = false; touch_dragged = true;
                scroll_repeat = ScrollRepeat{};
                lock_previous_frame = lock_last_activity = wall_us();
                continue;
            }
        }
        kb.configure(settings.custom_keyboard, settings.isDark(), settings.keyboard_languages, settings.keyboard_split);
        int64_t now = sceKernelGetSystemTimeWide();
        if (font_pending) {
            const auto status = rich_text_prepare_custom_font(requested_font);
            if (status == LanguageFontStatus::Failed) {
                font_pending = false;
                settings_error = i18n::text("settings.custom_font_failed");
            } else if (status == LanguageFontStatus::Ready) {
                const std::string previous = settings.font_file;
                if (!rich_text_set_custom_font(requested_font)) settings_error = i18n::text("settings.custom_font_failed");
                else {
                    settings.font_file = requested_font;
                    if (!settings.save()) {
                        settings.font_file = previous;
                        rich_text_set_custom_font(previous);
                        settings_error = i18n::text("settings.save_failed");
                    } else { settings_error.clear(); theme_picker.close(); }
                }
                settings_label_marquee.key = settings_value_marquee.key = -1;
                font_pending = false;
            }
        }
        if (language_pending) {
            const std::string code = i18n::resolve(requested_language);
            const char* path = language_font_path_for(code);
            auto status = rich_text_prepare_language_font(path);
            if (code == "ja" && status == LanguageFontStatus::Ready)
                status = rich_text_japanese_font_status();
            if (status == LanguageFontStatus::Failed) {
                language_pending = false;
                settings_error = i18n::text("settings.font_load_failed");
            } else if (status == LanguageFontStatus::Ready) {
                if (rich_text_set_language_font(path)) {
                    i18n::select(requested_language);
                    settings.language = requested_language;
                    tg.refreshLanguage();
                    net.refreshLanguage(); // Same device state, new label in this frame.
                    for (auto* text : {&audio_error, &circle_error, &browser_error}) text->refresh();
                    folder_picker.error.refresh();
                    upload_picker.error.refresh(); upload_picker.info.refresh();
                    for (auto& entry : upload_picker.entries) entry.label.refresh();
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else settings_error = i18n::text("settings.font_load_failed");
                language_pending = false;
                settings_label_marquee.key = settings_value_marquee.key = -1;
            }
        }
        if (!update_auto_checked && now >= update_check_due) {
            update_auto_checked = true;
            // The updater connects to GitHub directly, not through the Telegram proxy: with a proxy on,
            // it only runs when the user asks for it (Settings > Update), never by itself.
            if (settings.check_updates && net_ok && !settings.proxy_enabled && !network_held) { update_auto_install = true; updater.check(); }
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
                    !recorder.processing() && !circle_recorder.active() && !recorder.active() && !tg.currentChatId()) {
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
        if (now >= next_header_poll) {
            SceDateTime local_time{};
            if (sceRtcGetCurrentClockLocalTime(&local_time) >= 0)
                std::snprintf(header_clock, sizeof(header_clock), "%02u:%02u",
                    (unsigned)local_time.hour, (unsigned)local_time.minute);
            else std::snprintf(header_clock, sizeof(header_clock), "--:--");
            battery_percent = scePowerGetBatteryLifePercent();
            battery_charging = scePowerIsBatteryCharging() > 0;
            next_header_poll = now + 1000000;
        }
        bool reaction_resume = false;
        {
            const uint64_t wall = wall_us();
            bool left_app = lock_previous_frame && wall > lock_previous_frame + kLeftAppUs;
            SceAppMgrSystemEvent system_event{};
            // Drain queued notifications so a resume event is handled before drawing restored media.
            for (int event = 0; event < 32; ++event) {
                system_event = {};
                if (sceAppMgrReceiveSystemEvent(&system_event) < 0 || !system_event.systemEvent) break;
                if (system_event.systemEvent == SCE_APPMGR_SYSTEMEVENT_ON_RESUME) left_app = true;
            }
            reaction_resume = left_app;
            if (left_app && settings.spoilerHideApp()) {
                invalidate_spoiler_view(spoiler_visibility.appLeft(true), true);
            }
            lock_previous_frame = wall;
            const uint64_t idle_limit = (uint64_t)AppSettings::lockMinutes(settings.auto_lock) * 60000000ULL;
            // Recording, playback and typing in the system keyboard are activity without button presses.
            const auto circle_state = circle_recorder.state();
            const bool busy = recorder.recording() || recorder.processing() || recorder.previewPlaying() || animation.playing() || video_note_animation.playing() || voice.playing() || kb.active() ||
                (circle_playback && circle_playback->playing()) ||
                circle_state == VideoNoteRecorder::State::Recording || circle_state == VideoNoteRecorder::State::Encoding;
            if (busy) lock_last_activity = wall;
            if (left_app && settings.lock_on_leave) lock_pending = true;
            if (idle_limit && wall > lock_last_activity + idle_limit) lock_pending = true;
            if (!vault::unlocked() || vault::disabling()) lock_pending = false;
            // The system keyboard is a dialog on top of the app (closing it would lose the typed text).
            if (lock_pending && !kb.active()) { lock_pending = false; lock_app(); }
        }
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~old_buttons;
        auto scroll_direction = scroll_input.sample(pad.buttons & SCE_CTRL_UP,
                                                         pad.buttons & SCE_CTRL_DOWN, pad.ly);
        scroll_repeat.beginFrame();
        unsigned physical_pressed = pressed;
        bool navigation_handled = false;
        auto repeat_scroll = [&](ScrollContext context, double page_steps) {
            if (calendar_open) return;
            if ((tg.botWebApp().phase != TelegramClient::BotWebApp::Closed || (bot_panel && context != ScrollContext::Bots)) || navigation_handled || compose_wheel.busy()) return;
            navigation_handled = true;
            constexpr unsigned arrows = SCE_CTRL_UP | SCE_CTRL_DOWN;
            if (kb.active() || seek_open || seek_touch_release || confirm_leave ||
                unsupported_video_prompt || spoiler_prompt || (physical_pressed & ~arrows)) return;
            const unsigned held = pad.buttons & arrows;
            if (held == arrows) pressed &= ~arrows;
            const int repeated = scroll_repeat.update(scroll_direction.direction, scroll_direction.fresh,
                                                       (int)context, page_steps, now, settings.scroll_speed);
            const int step = scroll_direction.fresh ? scroll_direction.direction : repeated;
            if (step) pressed |= step < 0 ? SCE_CTRL_UP : SCE_CTRL_DOWN;
        };
        const bool button_action = pressed != 0 || scroll_direction.fresh;
        SceTouchData touch{};
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        const bool photo_stick_active = photo_open && (PhotoView::stickAxis(pad.lx) != 0 ||
            PhotoView::stickAxis(pad.ly) != 0 || PhotoView::stickAxis(pad.ry) != 0);
        const bool webapp_stick_active =
#if VITAGRAM_WEBKIT
            tg.botWebApp().phase == TelegramClient::BotWebApp::Ready && !webapp_keyboard.active() &&
            (PhotoView::stickAxis(pad.lx) != 0 || PhotoView::stickAxis(pad.ly) != 0);
#else
            false;
#endif
        const bool interaction = button_action || photo_stick_active || webapp_stick_active || scroll_direction.direction || (pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN)) || touch.reportNum || touch_down || (old_buttons & ~pad.buttons);
        if (interaction) media_active_at = now;
        if (interaction) lock_last_activity = wall_us();
        const int64_t input_sampled = interaction ? performance::now() : 0;
        if (interaction) tg.prioritizeInteraction(now);
        update_audio_bar();
        if (bg_show || bg_touch) {
            const bool down = touch.reportNum > 0;
            const float tx = down ? touch.report[0].x / 2.f : bg_last_x, ty = down ? touch.report[0].y / 2.f : bg_last_y;
            if (bg_show && down && !bg_prev_touch && !touch_down) {
                bg_touch_file = bg_media.file;
                if (bg_media.circle && !bg_volume_open && !bg_bar.panel().contains(tx, ty) && std::hypot(tx - bg_media.x, ty - bg_media.y) <= kBgRadius) {
                    bg_touch = true; bg_touch_bar = false; bg_touch_moved = false;
                    bg_touch_dx = bg_media.x - tx; bg_touch_dy = bg_media.y - ty; bg_start_x = tx; bg_start_y = ty;
                } else if (bg_bar.panel().contains(tx, ty) || bg_volume_open) {
                    bg_touch = true; bg_touch_bar = true;
                    bg_control = bg_bar.hit(tx, ty, bg_volume_open);
                    bg_touch_file = bg_media.file;
                    if (bg_control == audio_bar::None || (!bg_bar.popup().contains(tx, ty) && bg_control != audio_bar::Volume)) bg_volume_open = false;
                }
            }
            if (bg_touch) {
                if (down) {
                    if (bg_touch_bar && bg_show && bg_touch_file == bg_media.file) {
                        if (bg_control == audio_bar::Seek) bg_seek_target = bg_bar.seekAt(tx, bg_duration(), bg_limit());
                        else if (bg_control == audio_bar::VolumeTrack) bg_set_volume(bg_bar.volumeAt(tx));
                    } else if (!bg_touch_bar && bg_show && bg_touch_file == bg_media.file) {
                        if (std::hypot(tx - bg_start_x, ty - bg_start_y) > 6.f) bg_touch_moved = true;
                        if (bg_touch_moved) {
                            bg_media.x = std::max(kBgRadius, std::min(960.f - kBgRadius, tx + bg_touch_dx));
                            bg_media.y = std::max(kBgRadius, std::min(bg_bar.top() - kBgRadius - 4, ty + bg_touch_dy));
                        }
                    }
                    bg_last_x = tx; bg_last_y = ty;
                } else {
                    bg_touch = false;
                    if (bg_show && bg_touch_bar && bg_touch_file == bg_media.file) {
                        if (bg_control == audio_bar::Seek) bg_seek(bg_seek_target);
                        else if (bg_control == bg_bar.hit(tx, ty, bg_volume_open)) bg_action(bg_control);
                    } else if (bg_show && !bg_touch_bar && !bg_touch_moved && bg_touch_file == bg_media.file) bg_action(audio_bar::Play);
                    bg_control = audio_bar::None;
                }
                touch.reportNum = 0;
            }
            bg_prev_touch = down;
        } else bg_prev_touch = touch.reportNum > 0;
        const unsigned shoulders = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
        const bool player_chord = bg_show && bg_shoulders.update(pad.buttons,pressed,shoulders);
        if (!bg_show) bg_shoulders = {};
        else physical_pressed = (physical_pressed&~shoulders) | (pressed&shoulders);
        if (player_chord) { bg_focus = !bg_focus; bg_volume_open = false; }
        if (bg_focus || player_chord) {
            if (!player_chord) {
                if (pressed & SCE_CTRL_LEFT) bg_focus_control = audio_bar::Control((int(bg_focus_control)+audio_bar::Count-1)%audio_bar::Count);
                if (pressed & SCE_CTRL_RIGHT) bg_focus_control = audio_bar::Control((int(bg_focus_control)+1)%audio_bar::Count);
                const int adjust = (pressed & SCE_CTRL_UP) ? 1 : (pressed & SCE_CTRL_DOWN) ? -1 : 0;
                if (adjust) {
                    if (bg_focus_control == audio_bar::Volume) { bg_volume_open = true; bg_set_volume(bg_volume+adjust*10); }
                    else if (bg_focus_control == audio_bar::Speed && !bg_media.circle) voice.setSpeed(audio_bar::nextSpeed(voice.speed(), adjust));
                    else bg_seek(bg_position()+adjust*int64_t(10000000));
                }
                if (pressed & SCE_CTRL_CROSS) bg_action(bg_focus_control);
                if (pressed & SCE_CTRL_CIRCLE) { bg_focus = false; bg_volume_open = false; bg_shoulders = {}; }
            }
            pressed = physical_pressed = 0; scroll_direction = {}; navigation_handled = true;
        }
        unsigned int released = old_buttons & ~pad.buttons;
        old_buttons = pad.buttons;
        int64_t selected_chat = chat_index >= 0 && chat_index < (int)tg.chats().size() ? tg.chats()[chat_index].id : 0;
        const auto history_selection = HistorySelection::capture(tg.messages(), message_index);
        int64_t previous_last = tg.messages().empty() ? 0 : tg.messages().back().id;
        bool was_at_bottom = message_index < 0 || message_index + 1 == (int)tg.messages().size();
        bool was_cleaning = tg.cacheMaintenance();
        bool was_ready = tg.ready();
        update_performance_context(scroll_direction.direction != 0);
        pre_input.finish();
        ui_stage.to(ui_diagnostics::Phase::Pump);
        const bool keyboard_interaction = kb.active()
#if VITAGRAM_WEBKIT
            || webapp_keyboard.active()
#endif
            ;
        if (keyboard_interaction) tg.prioritizeInteraction(now);
        tg.pump(interaction || keyboard_interaction);
#if VITAGRAM_WEBKIT
        if (tg.botWebApp().phase == TelegramClient::BotWebApp::Closed && (webapp_confirmed || webapp_view.active())) reset_webapp_view();
        webapp_view.pump();
#endif
        update_audio_bar();
        ui_stage.to(ui_diagnostics::Phase::Frame);
        observe_spoiler_chat();
        if (tg.takeLinkChatOpened()) {
            if (about_chat_pending) {
                settings_open = about_open = false; about_chat_pending = false;
                settings_error.clear();
            }
            post_open = photo_open = video_fullscreen = false;
            message_index = -1; photo_index = 0; post_text_scroll = 0;
            animation.setSource("");
        }
        performance::SlowScope post_pump(performance::SlowPhase::Ui,"post_pump");
        performance::SlowScope post_step(performance::SlowPhase::Ui,"post_state");
        if (!startup_ready_measured && tg.ready()) {
            startup_ready_measured = true;
            performance::observeAlways(performance::Metric::StartupReady, performance::now()-startup_time);
        }
        if (!startup_live_list_measured && tg.liveListShown()) {
            startup_live_list_measured = true;
            performance::observeAlways(performance::Metric::StartupLiveList, performance::now()-startup_time);
        }
        if (!startup_window_closed && (tg.liveListShown() || sceKernelGetSystemTimeWide() >= startup_clock_until)) {
            if (startup_clock_raised) scePowerSetArmClockFrequency(startup_clock_base);
            startup_clock_raised = false; startup_window_closed = true;
            cpu_scheduler::pinParser(false);
            event_stats::stop(); // Also reported when no live list arrives (login screen, offline).
        }
        if (was_ready && !tg.ready()) {
            recorder.cancel(); circle_playback.reset(); circle_recorder.cancel();
            animation.setSource(""); video_note_animation.setSource(""); voice.setSource(""); bg_media.live = false;
            post_open = photo_open = video_fullscreen = false;
        }
        if (!tg.currentChatId()) reactions_open = false;
        if (!tg.currentChatId() || !tg.ready()) { stickers_open = false; upload_picker.close(); }
        if (gifs_open && (!tg.ready() || !tg.gifsCurrent())) {
            gifs_open = false; tg.closeGifs();
            if (gif_input) { kb.close(); gif_input = false; }
        }
        if ((was_cleaning && !tg.cacheMaintenance()) || (was_ready && !tg.ready())) {
            vita2d_wait_rendering_done(); textures.clear();
        }
        post_step.next("post_scheduler");
        cpu_scheduler::tick();
        post_step.next("post_selection");
        if (selected_chat) {
            for (size_t i = 0; i < tg.chats().size(); ++i)
                if (tg.chats()[i].id == selected_chat) { chat_index = (int)i; break; }
        }
        chat_index = std::max(0, std::min(chat_index, (int)tg.chats().size() - 1));
        if (tg.takeChatTopRequest() && !tg.currentChatId()) { chat_index = 0; chat_scroll.reset(); }
        const int restored_message = history_selection.find(tg.messages());
        if (restored_message >= 0) message_index = restored_message;
        else if (history_selection.message_id && post_open) {
            post_open = photo_open = video_fullscreen = reactions_open = false;
            animation.setSource("");
        }
        if (was_at_bottom && !post_open && !photo_open && !tg.messages().empty() &&
            (tg.messages().back().outgoing || tg.followingBotReplies()) && tg.messages().back().id != previous_last)
            message_index = (int)tg.messages().size() - 1;
        if (tg.takeBotReplyFocus()) {
            message_index = (int)tg.messages().size() - 1;
            history_scroll = {};
            combined_list_focus = false;
        }
        if (const int64_t target = tg.takeMessageJump()) {
            history_scroll = {};
            for (size_t i = 0; i < tg.messages().size(); ++i) if (tg.messages()[i].id == target) {
                message_index = (int)i;
                post_open = tg.messageJumpOpensPost(); photo_open = video_fullscreen = reactions_open = false;
                photo_index = 0; post_text_scroll = 0;
                animation.setSource("");
                break;
            }
        }

        post_step.next("post_composition");
        radial::Context compose_context;
        compose_context.chat = tg.currentChatId(); compose_context.topic = tg.currentTopicId();
        compose_context.thread = tg.currentThreadId(); compose_context.reply = tg.replyTarget();
        if (message_index >= 0 && message_index < (int)tg.messages().size() && !tg.messages()[message_index].service_message)
            compose_context.selected = tg.messages()[message_index].id;
        if (radial_flow && (!compose_origin.samePlace(compose_context) || !tg.ready() ||
            (compose_origin.reply && tg.replyTarget() != compose_origin.reply))) {
            upload_picker.close(); stickers_open = gifs_open = false; tg.closeGifs(); recorder.cancel();
            if (tg.inputKind() == InputKind::Message || upload_text_input || gif_input) kb.close();
            upload_text_input = gif_input = false; tg.cancelInput(); radial_flow = false;
        }
        history_view = chat_view::layout(combined_visible(), !tg.folders().empty());
        if (!tg.currentChatId() || message_index < 0) history_scroll = {};
        const bool drawer_available = history_view.combined && !chat_modal_open() && !tg.folders().empty() && !chat_menu_open && !chat_menu_waiting;
        if (!drawer_available) folder_drawer.reset();
        if (drawer_available) {
            const int raw_x = touch.reportNum ? touch.report[0].x/2 : 0;
            const int raw_y = touch.reportNum ? touch.report[0].y/2 : 0;
            int chosen = -1;
            const bool drawer_touch = folder_drawer.touch(touch.reportNum != 0, raw_x, raw_y,
                (int)tg.folders().size(), now, chosen);
            auto choose_folder = [&](int index) {
                tg.selectFolder(tg.folders()[index].id); chat_index = 0; chat_scroll.reset();
                combined_list_focus = true; search_bar_focus = false;
            };
            if (chosen >= 0) choose_folder(chosen);
            if (drawer_touch) {
                touch.reportNum = 0; touch_down = false; touch_dragged = touch_blocked = true;
                chat_drag = history_drag = folder_drag = false; touch_pane = chat_view::Pane::None; scrub_kind = 0;
            }
            if (folder_drawer.open) {
                repeat_scroll(ScrollContext::FolderDrawer, chat_view::FolderDrawer::rows);
                if (pressed & SCE_CTRL_UP) --folder_drawer.focus;
                if (pressed & SCE_CTRL_DOWN) ++folder_drawer.focus;
                folder_drawer.reveal((int)tg.folders().size());
                if (pressed & SCE_CTRL_CROSS) { choose_folder(folder_drawer.focus); folder_drawer.show(false,now); }
                if (pressed & (SCE_CTRL_RIGHT | SCE_CTRL_CIRCLE | SCE_CTRL_START)) folder_drawer.show(false,now);
                pressed = released = 0; scroll_direction.direction = 0;
            } else if (combined_list_focus && (pressed & SCE_CTRL_LEFT)) {
                for (int i=0; i<(int)tg.folders().size(); ++i)
                    if (tg.folders()[i].id == tg.selectedFolder()) folder_drawer.focus = i;
                folder_drawer.reveal((int)tg.folders().size());
                folder_drawer.show(true,now); pressed &= ~SCE_CTRL_LEFT;
            }
        }
        if (history_view.combined && !tg.folders().empty())
            for (unsigned i = 0; i < touch.reportNum; ++i)
                touch.report[i].x = std::max(0, (int)touch.report[i].x - 2*chat_view::FolderDrawer::contentShift(touch.report[i].y/2, drawer_draw_shift));
        if (!tg.currentChatId() && !tg.topicsOpen()) combined_list_focus = true;
        const bool compose_allowed = !folder_drawer.open && !folder_drawer.owned && !tg.historyAnchorPending() && !(history_view.combined && combined_list_focus) && !bot_panel && tg.botWebApp().phase == TelegramClient::BotWebApp::Closed && !radial_flow && tg.canPostCurrent() && !tg.topicsOpen() &&
            !post_open && !photo_open && !video_fullscreen && !settings_open && !chat_menu_open &&
            !reactions_open && !stickers_open && !gifs_open && !upload_picker.active() && !kb.active() &&
            !circle_recorder.active() && !recorder.active() && !seek_open && !seek_touch_release &&
            !confirm_leave && !spoiler_prompt && !unsupported_video_prompt && pending_web_link.empty() &&
            tg.inputKind() == InputKind::None;
        radial::Input wheel_input;
        wheel_input.allowed = compose_allowed;
        wheel_input.held = pad.buttons & SCE_CTRL_SQUARE;
        wheel_input.pressed = physical_pressed & SCE_CTRL_SQUARE;
        wheel_input.circle = physical_pressed & SCE_CTRL_CIRCLE;
        wheel_input.left = physical_pressed & SCE_CTRL_LTRIGGER;
        wheel_input.right = physical_pressed & SCE_CTRL_RTRIGGER;
        const int64_t wheel_now = sceKernelGetSystemTimeWide();
        wheel_input.interrupted = reaction_resume || wheel_now - now > 1000000 ||
            (physical_pressed & ~(SCE_CTRL_SQUARE | SCE_CTRL_CIRCLE | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER));
        wheel_input.stick_x = pad.lx; wheel_input.stick_y = pad.ly; wheel_input.context = compose_context;
        if (compose_wheel.busy() && compose_wheel.mode() == radial::Mode::Reply) {
            const auto& messages = tg.messages();
            if (std::none_of(messages.begin(), messages.end(), [&](const MessageItem& item) { return item.id == compose_wheel.reply(); }))
                wheel_input.interrupted = true;
        }
        const auto wheel_result = compose_wheel.update(wheel_input, wheel_now);
        if (compose_wheel.visible() && !compose_labels_ready) {
            auto prepare = [&](radial::Label& label, const std::string& text, int width, float scale) {
                label.text = fit_text(font, text, width, scale);
                label.width = rich_text_width(font, scale, label.text.c_str());
            };
            const char* keys[] = {"upload.media", "gif.menu", "upload.file", "radial.voice", "sticker.title"};
            const char* details[] = {"radial.media_detail", "radial.gif_detail", "radial.file_detail", "radial.voice_detail", "radial.sticker_detail"};
            for (int i = 0; i < 5; ++i) {
                prepare(compose_labels.petals[i], i == 1 ? "GIF" : tr(keys[i]), 148, 0.57f);
                prepare(compose_labels.details[i], tr(details[i]), 148, 0.43f);
            }
            prepare(compose_labels.modes[0], tr("radial.new_message"), 188, 0.67f);
            prepare(compose_labels.modes[1], tr("radial.reply"), 188, 0.67f);
            prepare(compose_labels.controls[0], tr("radial.left"), 40, 0.54f);
            prepare(compose_labels.controls[1], tr("radial.right"), 40, 0.54f);
            prepare(compose_labels.gif, "GIF", 36, 0.55f);
            prepare(compose_labels.text, tr("radial.message_hint"), 172, 0.57f);
            prepare(compose_labels.author, "", 172, 0.5f); prepare(compose_labels.preview, "", 168, 0.43f);
            for (const auto& item : tg.messages()) if (item.id == compose_wheel.reply()) {
                prepare(compose_labels.author, tg.senderName(item), 172, 0.5f);
                std::string preview = item.text.empty() ? item.media_label : item.text;
                if (spoiler_locked(item) || (!spoiler_visibility.revealed(tg.currentChatId(), item.id) &&
                    std::any_of(item.styles.begin(), item.styles.end(), [](const StyleSpan& span) { return span.style & TextSpoiler; })))
                    preview = tr("spoiler.title");
                std::replace(preview.begin(), preview.end(), '\n', ' ');
                prepare(compose_labels.preview, preview, 168, 0.43f);
                break;
            }
            compose_labels_ready = true;
        }
        if (!compose_wheel.busy()) compose_labels_ready = false;
        if (wheel_result.consume) {
            pressed = released = 0; navigation_handled = true; scroll_repeat = ScrollRepeat{};
            touch.reportNum = 0; touch_down = false; touch_dragged = touch_blocked = true; scrub_kind = 0;
        }
        if (wheel_result.execute && tg.setCompositionReply(wheel_result.reply)) {
            radial_flow = true; compose_origin = wheel_result.context; compose_origin.reply = wheel_result.reply;
            switch (wheel_result.action) {
                case radial::Action::Text: tg.requestCompose(); break;
                case radial::Action::Multimedia:
                case radial::Action::File:
                    upload_picker.openMenu();
                    upload_picker.chooseKind(wheel_result.action == radial::Action::File ? UploadPicker::Kind::File : UploadPicker::Kind::Media);
                    break;
                case radial::Action::Gif:
                    gifs_open = true; gif_index = 0; gif_next_page = -1; tg.searchGifs(); break;
                case radial::Action::Voice: open_audio(); break;
                case radial::Action::Stickers:
                    stickers_open = true; sticker_pack_menu = true; sticker_index = 0; sticker_pack = 1; tg.openStickers(); break;
            }
        }
        post_step.next("post_reactions");
        const auto& reaction_messages = tg.messages();
        const bool reaction_context = !folder_drawer.open && !folder_drawer.owned && !(history_view.combined && combined_list_focus) && !bot_panel && tg.botWebApp().phase == TelegramClient::BotWebApp::Closed && tg.currentChatId() && !tg.topicsOpen() &&
            message_index >= 0 && message_index < (int)reaction_messages.size() &&
            !reaction_messages[message_index].service_message && !photo_open && !video_fullscreen &&
            !settings_open && !chat_menu_open && !compose_wheel.busy() && !wheel_result.consume && !reactions_open && !stickers_open && !gifs_open &&
            !upload_picker.active() && !folder_picker.active && !kb.active() &&
            !circle_recorder.active() && !recorder.active() && !seek_open && !seek_touch_release &&
            !confirm_leave && !spoiler_prompt && !unsupported_video_prompt && pending_web_link.empty() &&
            tg.inputKind() == InputKind::None;
        const auto reaction_press_result = reaction_press.update(reaction_context,
            pad.buttons & SCE_CTRL_CROSS, physical_pressed & SCE_CTRL_CROSS,
            reaction_resume || (physical_pressed & ~SCE_CTRL_CROSS) || scroll_direction.direction || touch.reportNum || touch_down,
            tg.currentChatId(), reaction_context ? reaction_messages[message_index].id : 0,
            tg.currentTopicId(), now);
        if (reaction_press_result.consume) pressed &= ~SCE_CTRL_CROSS;
        if (reaction_press_result.short_press) pressed |= SCE_CTRL_CROSS;
        if (reaction_press_result.long_press) {
            reactions_open = true; reaction_index = 0;
            tg.loadReactions(reaction_messages[message_index].id, true);
        }
        const bool opening_message = !settings_open && tg.historyAnchorPending();
        static bool opening_touch_down = false;
        const bool opening_sidebar = history_view.combined && touch.reportNum &&
            (history_view.folderBandHit(touch.report[0].x/2, touch.report[0].y/2) ||
             (touch.report[0].x/2 < chat_view::sidebar && touch.report[0].y/2 >= history_view.searchTop()));
        const bool opening_touched = touch.reportNum && !opening_touch_down && !opening_sidebar &&
            (!history_view.combined || history_view.historyHit(touch.report[0].x/2, touch.report[0].y/2, 544));
        opening_touch_down = opening_message && touch.reportNum;
        if (opening_message && history_view.combined && (pressed & SCE_CTRL_LEFT)) {
            combined_list_focus = true; pressed &= ~SCE_CTRL_LEFT;
        }
        if (opening_message && (!history_view.combined || (touch.reportNum ? !opening_sidebar : !combined_list_focus))) {
            const bool touched = opening_touched;
            if (tg.historyAnchorFar() && ((pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) || touched))
                tg.acceptHistoryAnchorFar();
            else if (!tg.historyError().empty() && ((pressed & SCE_CTRL_CROSS) || touched)) tg.retryHistoryOpening();
            else if (pressed & SCE_CTRL_CIRCLE) tg.leaveChat();
            pressed = released = 0; touch_down = touch.reportNum != 0;
            touch.reportNum = 0; touch_dragged = touch_blocked = true; scrub_kind = 0;
            scroll_direction.direction = 0;
        }
        post_step.next("post_scroll_layout");
        const bool folder_tabs = !tg.folders().empty();
        const int chat_rows = folder_tabs && !history_view.combined ? ui_layout::chat_rows_folders : ui_layout::chat_rows_plain;
        const int chat_top = history_view.listTop();
        const int chat_row_height = (ui_layout::height-chat_top)/chat_rows;
        const int list_bottom = 544;
        chat_scroll.configure((int)tg.chats().size(), chat_row_height, list_bottom-chat_top);
        settings_scroll.configure(keyboard_settings ? keyboard_rows() : settings_menu::counts[settings_section], kSettingsRowHeight, kSettingsViewport);
        if (history_view.combined || (!tg.currentChatId() && tg.canBrowse() && !settings_open && !search_open &&
                !tg.topicsOpen() && !recorder.active() && !stickers_open && !gifs_open)) {
            std::vector<int64_t> ids; ids.reserve(tg.chats().size());
            for (const auto& chat : tg.chats()) ids.push_back(chat.id);
            chat_motion.update(ids, chat_row_height, tg.selectedFolder(), now);
        }
        auto hit_chat = [&](int x, int y) {
            if (y >= list_bottom) return -1;
            for (int i = chat_scroll.end()-1; i >= chat_scroll.first(); --i) {
                const int offset = chat_scroll.offset() - (int)std::lround(chat_motion.offset((size_t)i, now));
                if (history_view.listHit(x, y, chat_top, chat_row_height, offset) && (y-chat_top+offset)/chat_row_height == i) return i;
            }
            return -1;
        };
        const int folder_max_scroll = std::max(0, ((int)tg.folders().size() - 5) * kFolderStep);
        folder_scroll = std::max(0, std::min(folder_scroll, folder_max_scroll));
        if (seek_open) {
            bool apply = pressed & SCE_CTRL_CROSS;
            bool cancel = pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START);
            if (!seek_source_current()) { apply = false; cancel = true; }
            if (pressed & SCE_CTRL_LEFT) seek_time.select(-1);
            if (pressed & SCE_CTRL_RIGHT) seek_time.select(1);
            if (pressed & SCE_CTRL_UP) seek_time.adjust(1);
            if (pressed & SCE_CTRL_DOWN) seek_time.adjust(-1);
            if (touch.reportNum && !seek_touching) {
                const int x = touch.report[0].x / 2, y = touch.report[0].y / 2;
                for (int field = 0; field < 3; ++field) if (x >= 260 + field * 150 && x < 388 + field * 150) {
                    seek_time.field = field;
                    if (y >= 180 && y < 220) seek_time.adjust(1);
                    else if (y >= 280 && y < 320) seek_time.adjust(-1);
                }
                if (y >= 382 && y < 426) {
                    if (x >= 220 && x < 450) apply = true;
                    else if (x >= 510 && x < 740) cancel = true;
                }
            }
            seek_touching = touch.reportNum != 0;
            if (apply || cancel) {
                seek_touch_release = touch.reportNum != 0;
                finish_seek(apply && !cancel);
            }
            pressed = released = 0; touch.reportNum = 0; touch_down = false; scrub_kind = 0;
        }
        else if (seek_touch_release) {
            // Applying on touch-down must not turn its release into a tap on
            // the video or on a control that used to be underneath the dialog.
            seek_touch_release = touch.reportNum != 0;
            touch.reportNum = 0; touch_down = false; scrub_kind = 0;
        }
        post_step.next("post_touch");
        bool orientation_open = photo_open;
#if VITAGRAM_WEBKIT
        orientation_open = orientation_open || webapp_view.active();
#endif
        photo_view.updateMotion(orientation_open && settings.gyroscope_enabled, sceKernelGetProcessTimeWide(), touch.reportNum != 0 || photo_stick_active);
        bool photo_gesture = photo_open && photo_view.touch(touch);
        bool touching = touch.reportNum > 0;
        bool tap = false;
        const bool webapp_touch_control = scrub_kind != 0;
        const bot_buttons::PanelGeometry bot_geometry {history_view.left()};
        bool bot_touch_owned = bot_panel != 0 || tg.botWebApp().phase != TelegramClient::BotWebApp::Closed;
        if (touching) {
            touch_x = touch.report[0].x / 2;
            touch_y = touch.report[0].y / 2;
            bot_touch_owned = bot_panel || tg.botWebApp().phase != TelegramClient::BotWebApp::Closed || (chat_ui_visible() && tg.currentChatId() && !post_open && !photo_open &&
                bot_buttons::toolbar_hit(!tg.botMenu().empty(), !tg.botKeyboard().buttons.empty(), touch_x, touch_y, history_view.left()));
            if (!touch_down) {
                touch_start_x = touch_x; touch_start_y = touch_step_y = touch_y;
                touch_dragged = false;
                scrub_kind = 0;
                touch_blocked = kb.active() || (!circle_recorder.active() && !recorder.active() && !settings_open && tg.canBrowse() && tg.inputKind() != InputKind::None);
                folder_drag = !chat_modal_open() && !bot_touch_owned && !chat_menu_open && !chat_menu_waiting && !search_open && pending_web_link.empty() && !circle_recorder.active() && !recorder.active() && !stickers_open && !gifs_open && !settings_open && folder_tabs && (!tg.currentChatId() || history_view.combined) && history_view.folderBandHit(touch_start_x, touch_start_y);
                folder_touch_x = touch_x;
                const bool pane_touch = !chat_modal_open() && (!history_view.combined || history_view.historyHit(touch_x, touch_y, 544));
                if (pane_touch && !touch_blocked && !bot_touch_owned && !chat_menu_open && !upload_picker.active() && delete_hit.contains(touch_x, touch_y)) {
                    scrub_kind = 6; delete_touch_message = delete_hit_message; delete_touch_chat = tg.currentChatId();
                }
                else if (pane_touch && !touch_blocked && !bot_touch_owned && reply_hit.contains(touch_x, touch_y)) scrub_kind = 5;
                else if (pane_touch && !touch_blocked && !bot_touch_owned && scrub_play.contains(touch_x, touch_y)) scrub_kind = 4;
                else if (pane_touch && !touch_blocked && !bot_touch_owned && scrub_bar.contains(touch_x, touch_y)) scrub_kind = 2;
                else if (pane_touch && !touch_blocked && !bot_touch_owned && scrub_voice.contains(touch_x, touch_y)) scrub_kind = 1;
                else if (pane_touch && !touch_blocked && !bot_touch_owned && scrub_ring.on) {
                    const float distance = std::hypot(touch_x - scrub_ring.x, touch_y - scrub_ring.y);
                    if (distance >= scrub_ring.w - 28 && distance <= scrub_ring.w + 24) scrub_kind = 3;
                }
                // Capture the pane once: crossing the divider must not redirect a drag.
                touch_pane = !chat_modal_open() && !touch_blocked && !bot_touch_owned && !chat_menu_open && !chat_menu_waiting ?
                    history_view.scrollPane(touch_start_x, touch_start_y, history_bottom) : chat_view::Pane::None;
                if (touch_pane == chat_view::Pane::History && !tg.currentChatId() && !tg.topicsOpen()) touch_pane = chat_view::Pane::None;
                if (touch_pane != chat_view::Pane::None) combined_list_focus = touch_pane == chat_view::Pane::Chats;
                chat_drag = !chat_menu_open && !chat_menu_waiting && !bot_panel && tg.botWebApp().phase == TelegramClient::BotWebApp::Closed && !search_open && pending_web_link.empty() && !circle_recorder.active() && !recorder.active() && !stickers_open && !gifs_open && !settings_open &&
                    !reactions_open && !upload_picker.active() && (!tg.currentChatId() || history_view.combined) && tg.canBrowse() &&
                    !touch_blocked && (history_view.combined ? touch_pane == chat_view::Pane::Chats :
                        touch_start_y >= chat_top && touch_start_y < list_bottom &&
                        touch_start_x >= 10 && touch_start_x < 10 + history_view.listWidth());
                chat_touch_y = touch_y;
                history_drag = (history_view.combined ? touch_pane == chat_view::Pane::History :
                    history_view.historyHit(touch_start_x, touch_start_y, history_bottom)) &&
                    !chat_menu_open && !chat_menu_waiting && tg.currentChatId() && !tg.historyAnchorPending() &&
                    !post_open && !photo_open && !video_fullscreen && !tg.topicsOpen() && !scrub_kind && !bot_panel &&
                    tg.botWebApp().phase == TelegramClient::BotWebApp::Closed && !search_open && pending_web_link.empty() &&
                    !circle_recorder.active() && !recorder.active() && !stickers_open && !gifs_open && !settings_open &&
                    !reactions_open && !upload_picker.active() && !touch_blocked;
            }
            if (std::abs(touch_y - touch_start_y) > 14 || std::abs(touch_x - touch_start_x) > 14) touch_dragged = true;
            if (scrub_kind >= 1 && scrub_kind <= 3) {
                // The finger sets the position: along the bar/waveform, or by angle around the ring (clockwise from the top).
                float fraction = 0;
                if (scrub_kind == 1) fraction = (touch_x - history_view.x(184)) / (float)history_view.width(592);
                else if (scrub_kind == 2) fraction = media_seek::track_fraction(touch_x, scrub_bar.x + 8, scrub_bar.w - 16);
                else {
                    float turn = std::atan2(touch_x - scrub_ring.x, -(touch_y - scrub_ring.y)) / 6.28318531f;
                    fraction = turn < 0 ? turn + 1.f : turn;
                }
                scrub_fraction = std::max(0.f, std::min(1.f, fraction));
            }
            if (folder_drag && !touch_blocked && touch_dragged) {
                folder_scroll = std::max(0, std::min(folder_max_scroll, folder_scroll + folder_touch_x - touch_x));
            }
            folder_touch_x = touch_x;
            if (chat_drag && touch_dragged &&
                std::abs(touch_y - touch_start_y) >= std::abs(touch_x - touch_start_x)) {
                if (history_view.combined) combined_list_focus = true;
                chat_scroll.drag(touch_y - chat_touch_y);
                if (chat_scroll.end() > chat_scroll.first())
                    chat_index = std::clamp(chat_index, chat_scroll.first(), chat_scroll.end()-1);
                if (chat_scroll.end() >= (int)tg.chats().size() - 2 && touch_y < chat_touch_y)
                    tg.loadMoreChats();
            }
            if (history_drag && touch_dragged &&
                std::abs(touch_y-touch_start_y) >= std::abs(touch_x-touch_start_x)) {
                combined_list_focus = false;
                const auto rows = history_layout(font, tg, message_index, touch_y-chat_touch_y);
                if (!rows.empty()) {
                    message_index = std::clamp(message_index, rows.front().index, rows.back().index);
                    history_scroll.select(tg.messages()[message_index].id);
                    if (rows.front().index <= 2 && touch_y > chat_touch_y) tg.loadOlder();
                    if (rows.back().index+1 >= (int)tg.messages().size() && touch_y < chat_touch_y) tg.loadNewer();
                }
            }
            // Include the initial movement once the tap threshold is crossed.
            if (history_drag && touch_dragged) chat_touch_y = touch_y;
            if (chat_drag && touch_dragged) chat_touch_y = touch_y;
            if ((!history_view.combined || touch_pane == chat_view::Pane::History) &&
                !bot_touch_owned && !history_drag && !chat_drag && !folder_drag && !touch_blocked && !scrub_kind && touch_start_y >= 38 && touch_start_y < 544 && !photo_open && !video_fullscreen &&
                std::abs(touch_y - touch_start_y) >= std::abs(touch_x - touch_start_x) &&
                std::abs(touch_y - touch_step_y) >= 32) {
                if (history_view.combined) combined_list_focus = false;
                pressed |= touch_y < touch_step_y ? SCE_CTRL_DOWN : SCE_CTRL_UP;
                touch_step_y = touch_y;
            }
        } else if (touch_down && !touch_blocked && !photo_gesture && scrub_kind) {
            // Releasing a control commits: seek (clamped to the real duration) or toggle playback.
            const int control = scrub_kind;
            scrub_kind = 0;
            if (control == 6) {
                if (!touch_dragged && tg.currentChatId() == delete_touch_chat && post_open &&
                    message_index >= 0 && message_index < (int)tg.messages().size() &&
                    tg.messages()[message_index].id == delete_touch_message)
                    tg.deleteMessage(delete_touch_message);
            }
            else if (control == 4) { if (!touch_dragged) pressed |= SCE_CTRL_CROSS; }
            else if (control == 5) {   // tap on the quote: open the original through the existing jump navigation
                if (!touch_dragged && message_index >= 0 && message_index < (int)tg.messages().size() && tg.messages()[message_index].reply_to_id &&
                    (!tg.messages()[message_index].reply_chat_id || tg.messages()[message_index].reply_chat_id == tg.currentChatId()))
                    tg.openPinnedMessage(tg.messages()[message_index].reply_to_id);
            }
            else if (message_index >= 0 && message_index < (int)tg.messages().size()) {
                const MessageItem& target = tg.messages()[message_index];
                if (control == 1 && target.voice && !target.voice_path.empty() && target.voice_duration > 0) {
                    const int64_t limit = std::max<int64_t>(0, (int64_t)target.voice_duration * 1000000 - 300000);
                    const int64_t wanted = std::min<int64_t>(limit, (int64_t)(scrub_fraction * target.voice_duration * 1e6));
                    if (std::llabs(wanted - (int64_t)(voice.positionSeconds() * 1e6)) > 250000) voice.seekTo(wanted);
                } else if (control != 1 && target.video && target.video_duration > 0 && post_animation().canSeek()) {
                    const int64_t limit = std::max<int64_t>(0, (int64_t)target.video_duration * 1000000 - 500000);
                    const int64_t wanted = std::min<int64_t>(limit, (int64_t)(scrub_fraction * target.video_duration * 1e6));
                    if (std::llabs(wanted - post_animation().positionUs()) > 250000) post_animation().seekTo(wanted);
                }
            }
        } else if (touch_down && !touch_blocked && !photo_gesture) {
            tap = !touch_dragged;
            if (!chat_modal_open() && !video_fullscreen && (photo_open || post_open) && std::abs(touch_x - touch_start_x) > 45 &&
                std::abs(touch_x - touch_start_x) > std::abs(touch_y - touch_start_y))
                pressed |= touch_x < touch_start_x ? SCE_CTRL_RIGHT : SCE_CTRL_LEFT;
        }
        post_step.next("post_picker_gestures");
        const bool theme_touch_release = theme_picker.active && touch_down && !touching;
        const bool sticker_context = stickers_open && !sticker_pack_menu && !settings_open &&
            !chat_menu_open && !kb.active() && tg.ready() &&
            sticker_index >= 0 && sticker_index < (int)tg.stickers().size();
        int touched_sticker = -1;
        if (sticker_context && !touch_blocked && touch_x >= 30 && touch_x < 930 && touch_y >= 130 && touch_y < 460) {
            const int index = (sticker_index / 18) * 18 + (touch_y - 130) / 110 * 6 + (touch_x - 30) / 150;
            if (index < (int)tg.stickers().size()) touched_sticker = index;
        }
        const bool preview_set_drag = sticker_preview_index >= 0 && sticker_context && touching;
        const bool preview_set_action = sticker_preview_index >= 0 && sticker_context &&
            tg.stickers()[sticker_index].set_id && touch_x >= 280 && touch_x < 680 && touch_y >= 430 && touch_y < 480;
        const bool open_preview_set = preview_set_action && touch_down && !touching;
        if (preview_set_action || preview_set_drag) touched_sticker = sticker_index;
        if (sticker_context && touching && !touch_down && touched_sticker >= 0)
            sticker_index = touched_sticker;
        const auto sticker_press_result = sticker_press.update(sticker_context,
            {tg.currentChatId(), tg.currentTopicId(), sticker_pack, sticker_index,
                sticker_context ? tg.stickers()[sticker_index].file : 0},
            pad.buttons & SCE_CTRL_CROSS, physical_pressed & SCE_CTRL_CROSS,
            touching, touching && !touch_down, touched_sticker,
            reaction_resume || scroll_direction.direction || (physical_pressed & ~SCE_CTRL_CROSS) ||
                ((touching || touch_down) && touch_dragged && !preview_set_action && !preview_set_drag), now);
        if (sticker_press_result.consume_cross) pressed &= ~SCE_CTRL_CROSS;
        if (sticker_press_result.consume_touch) tap = false;
        if (sticker_press_result.send) pressed |= SCE_CTRL_CROSS;
        sticker_preview_index = sticker_press_result.preview ? sticker_index : -1;
        if (open_preview_set) { open_selected_set(); tap = false; pressed &= ~SCE_CTRL_CROSS; }
        const auto& pack_messages = tg.messages();
        if (touching && !touch_down) {
            pack_touch_message = 0;
            if (post_open && message_index >= 0 && message_index < (int)pack_messages.size() && history_view.historyHit(touch_x, touch_y, 544))
                pack_touch_message = pack_messages[message_index].id;
            else if (!post_open && !pack_messages.empty() && history_view.historyHit(touch_x, touch_y, history_bottom))
                for (const auto& row : history_layout(font, tg, message_index))
                    if (touch_y >= row.top && touch_y < row.top + row.height - row.gap())
                        pack_touch_message = pack_messages[row.index].id;
        }
        int pack_touch_index = -1;
        for (size_t i = 0; i < pack_messages.size(); ++i)
            if (pack_messages[i].id == pack_touch_message) pack_touch_index = (int)i;
        const bool pack_message_context = !bot_panel && tg.botWebApp().phase == TelegramClient::BotWebApp::Closed && !touch_blocked && !scrub_kind && !stickers_open && !gifs_open && !reactions_open && !settings_open &&
            !chat_menu_open && !upload_picker.active() && !photo_open && !video_fullscreen && !tg.topicsOpen() &&
            !circle_recorder.active() && !recorder.active() && !kb.active() && pack_touch_index >= 0 &&
            (pack_messages[pack_touch_index].sticker_set_id || !pack_messages[pack_touch_index].custom_emoji_ids.empty());
        const auto pack_hold = message_pack_press.update(pack_message_context, touching,
            touching && !touch_down, touch_dragged || reaction_resume || physical_pressed,
            tg.currentChatId(), pack_touch_message, tg.currentTopicId(), now);
        if (pack_hold.long_press) { message_index = pack_touch_index; open_selected_set(); tap = false; }
        if (pack_hold.consume && !pack_hold.short_press) tap = false;
        const bool gif_context = gifs_open && !settings_open && !chat_menu_open && !kb.active() &&
            tg.ready() && tg.gifsCurrent() && gif_index >= 0 && gif_index < (int)tg.gifs().items.size();
        int touched_gif = -1;
        if (gif_context && !touch_blocked && touch_x >= 30 && touch_x < 930 && touch_y >= 130 && touch_y < 448) {
            const int index = gif_index / 12 * 12 + (touch_y - 130) / 106 * 4 + (touch_x - 30) / 225;
            if (index < (int)tg.gifs().items.size()) touched_gif = index;
        }
        if (gif_context && touching && !touch_down && touched_gif >= 0) gif_index = touched_gif;
        const auto gif_press_result = gif_press.update(gif_context,
            {tg.currentChatId(), tg.currentTopicId(), (int)tg.gifs().generation, gif_index,
                gif_context ? tg.gifs().items[gif_index].file : 0},
            pad.buttons & SCE_CTRL_CROSS, physical_pressed & SCE_CTRL_CROSS,
            touching, touching && !touch_down, touched_gif,
            reaction_resume || scroll_direction.direction || (physical_pressed & ~SCE_CTRL_CROSS) ||
                ((touching || touch_down) && touch_dragged), now);
        if (gif_press_result.consume_cross) pressed &= ~SCE_CTRL_CROSS;
        if (gif_press_result.consume_touch) tap = false;
        if (gif_press_result.send) pressed |= SCE_CTRL_CROSS;
        gif_preview_index = gif_press_result.preview ? gif_index : -1;
        if (touch_down && !touching && chat_ui_visible() &&
            (!tg.botMenu().empty() || !tg.botKeyboard().buttons.empty() || !bot_message_hits.empty()))
            webapp_diagnostics::record(webapp_diagnostics::Stage::TouchRelease,
                (touch_blocked ? 1 : 0) | (touch_dragged ? 2 : 0) | (webapp_touch_control ? 4 : 0));
        touch_down = touching;
        if (touching || pressed) decode_cooldown = 8;
        else if (decode_cooldown) --decode_cooldown;
        post_step.next("post_textures");
        textures.beginFrame(sticker_preview_index < 0 && gif_preview_index < 0 &&
            (interaction || decode_cooldown != 0 || kb.active() || circle_recorder.active() || recorder.active()));

        post_step.finish();
        post_pump.finish();
        ui_stage.to(ui_diagnostics::Phase::Input);
        performance::SlowScope input_actions(performance::SlowPhase::Ui,settings_open ? "input_settings" : photo_open ? "input_photo" : post_open ? "input_post" : "input_browser");
        // Dismiss previous results before handling input, so a new error from
        // this action can still appear, including one with the same text.
        if (button_action || pressed || tap || touching) {
            browser_error.clear(); browser_notice.clear();
            tg.dismissStatusNotices();
            if (!recorder.active()) {
                recorder.setStatus("");
            }
        }

        auto bot_markup = [&](int panel, int64_t message) -> bot_buttons::Markup {
            if (panel == 4) return {};
            if (panel == 1) return tg.botMenu();
            if (panel == 2) return tg.botKeyboard().buttons;
            for (const auto& item : tg.messages()) if (item.id == message) return item.buttons;
            return {};
        };
        auto open_bot_panel = [&](int panel, int64_t message) {
            if (panel == 1) webapp_diagnostics::record(webapp_diagnostics::Stage::MenuOpen);
            scrub_kind = 0; chat_drag = history_drag = folder_drag = false; touch_pane = chat_view::Pane::None;
            if (panel == 1 && bot_buttons::menu_app(tg.botMenu())) {
                if (tg.activateBotButton(0, 0, 0, false)) { bot_panel = 0; return; }
                bot_alert = tg.botStatus(); panel = 4;
            }
            bot_panel = panel; bot_focus = 0; bot_panel_message = message;
            bot_sheet_since = now; bot_sheet_closing = false;
            bot_panel_chat = tg.currentChatId(); bot_panel_topic = tg.currentTopicId();
            bot_snapshot = bot_markup(panel, message); bot_marquee.key = -1;
            if (bot_snapshot.empty() && panel != 4) bot_panel = 0;
            chat_menu_open = false; chat_menu_waiting = false;
        };
        auto close_bot_panel = [&] {
            if (bot_panel == 1 && !bot_sheet_closing) { bot_sheet_closing = true; bot_sheet_since = now; }
            else if (bot_panel != 1) bot_panel = 0;
        };
        const auto alert = chat_ui_visible() && !compose_wheel.busy() && !recorder.active() && !circle_recorder.active() ? tg.takeBotAlert() : std::string{};
        if (!alert.empty()) { bot_alert = alert; open_bot_panel(4, 0); }
        if (tg.botWebApp().phase != TelegramClient::BotWebApp::Closed) bot_panel = 0;
        const bool bot_chat = tg.botWebApp().phase == TelegramClient::BotWebApp::Closed &&
            !tg.historyAnchorPending() && chat_ui_visible() && tg.ready() && tg.currentChatId() &&
            !compose_wheel.busy() && !post_open && !photo_open && !video_fullscreen && !tg.topicsOpen() &&
            !stickers_open && !gifs_open && !chat_menu_open && !circle_recorder.active() &&
            !recorder.active() && !upload_picker.active() && pending_web_link.empty();
        if (calendar_open && (tg.currentChatId() != calendar_chat || tg.currentThreadId() != calendar_thread ||
            tg.currentTopicId() != calendar_topic || settings_open)) calendar_open = false;
        if (!calendar_open && tap && chat_ui_visible() && !post_open && !photo_open && !tg.topicsOpen() &&
            !chat_menu_open && tg.currentChatId() && history_view.historyHit(touch_x, touch_y, history_bottom)) {
            const auto rows = history_layout(font, tg, message_index);
            auto chip = history_date_chip(font, rows);
            int index = chip.hit(touch_x, touch_y) ? chip.index : -1;
            for (const auto& row : rows) if (row.day_top >= history_view.top &&
                touch_y >= row.day_top && touch_y < row.day_top+22 &&
                std::abs(touch_x-history_view.center()) < (rich_text_width(font, 0.55f, row.day.c_str())+24)/2)
                index = row.index;
            if (index >= 0) {
                calendar_today = local_calendar(time(nullptr));
                calendar = local_calendar(tg.messages()[index].date);
                calendar.moveMonth(0, calendar_today);
                calendar_chat = tg.currentChatId(); calendar_thread = tg.currentThreadId(); calendar_topic = tg.currentTopicId();
                calendar_open = true; tap = false;
            }
        }
        if (calendar_open) {
            int chosen = 0;
            if (pressed & SCE_CTRL_CIRCLE) calendar_open = false;
            if (pressed & SCE_CTRL_LTRIGGER) calendar.moveMonth(-1, calendar_today);
            if (pressed & SCE_CTRL_RTRIGGER) calendar.moveMonth(1, calendar_today);
            int delta = (pressed & SCE_CTRL_LEFT ? -1 : pressed & SCE_CTRL_RIGHT ? 1 :
                pressed & SCE_CTRL_UP ? -7 : pressed & SCE_CTRL_DOWN ? 7 : 0);
            calendar.day = std::clamp(calendar.day+delta, 1, ChatCalendar::days(calendar.year, calendar.month));
            if (pressed & SCE_CTRL_CROSS) chosen = calendar.day;
            if (tap) {
                if (touch_y >= 94 && touch_y < 138) {
                    if (touch_x >= 250 && touch_x < 310) calendar.moveMonth(-1, calendar_today);
                    if (touch_x >= 650 && touch_x < 710) calendar.moveMonth(1, calendar_today);
                }
                if (touch_x >= 250 && touch_x < 710 && touch_y >= 456 && touch_y < 500) calendar_open = false;
                chosen = calendar.hit(touch_x, touch_y);
            }
            if (chosen && calendar.enabled(chosen, calendar_today)) {
                SceDateTime date{};
                date.year = calendar.year; date.month = calendar.month; date.day = chosen;
                date.hour = 23; date.minute = 59; date.second = 59;
                SceRtcTick local{}, utc{}; time_t timestamp = 0;
                if (sceRtcGetTick(&date, &local) >= 0 && sceRtcConvertLocalTimeToUtc(&local, &utc) >= 0 &&
                    sceRtcSetTick(&date, &utc) >= 0 && sceRtcGetTime_t(&date, &timestamp) >= 0 && timestamp > 0 && timestamp <= INT32_MAX) {
                    tg.openChatDate((int32_t)std::min<int64_t>(timestamp, time(nullptr)));
                    calendar_open = false;
                }
            }
            tap = false; pressed = released = 0; scroll_direction = {}; navigation_handled = true;
        }
        if (tap && tool_window_open() && !kb.active() && chat_view::ToolWindow::backHit(touch_x, touch_y)) {
            pressed |= SCE_CTRL_CIRCLE; tap = false;
        }
        if (tap && bot_buttons::toolbar_hit(!tg.botMenu().empty(), !tg.botKeyboard().buttons.empty(), touch_x, touch_y, history_view.left()))
            webapp_diagnostics::record(webapp_diagnostics::Stage::ToolbarTap, bot_chat);
        if (bot_panel == 1 && bot_sheet_closing && now - bot_sheet_since >= bot_buttons::sheet_duration) {
            bot_panel = 0; pressed = released = 0; tap = false;
        }
        if (bot_panel) {
            auto current = bot_markup(bot_panel, bot_panel_message);
            if (!tg.ready() || tg.currentChatId() != bot_panel_chat || tg.currentTopicId() != bot_panel_topic ||
                !(current == bot_snapshot) || kb.active() || settings_open) bot_panel = 0;
            else if (!bot_sheet_closing || bot_panel != 1) {
                if (bot_panel != 4 && (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
                    const int64_t selected_message = message_index >= 0 && message_index < (int)tg.messages().size() ? tg.messages()[message_index].id : 0;
                    for (int step = 1; step <= 3; ++step) {
                        const int next = (bot_panel - 1 + ((pressed & SCE_CTRL_LTRIGGER) ? 3 - step : step)) % 3 + 1;
                        const int64_t source = next == 2 ? tg.botKeyboard().id : next == 3 ? selected_message : 0;
                        if (next == 3 && !source) continue;
                        if (!bot_markup(next, source).empty()) { open_bot_panel(next, source); break; }
                    }
                    current = bot_snapshot;
                    pressed = 0; tap = false;
                }
                repeat_scroll(ScrollContext::Bots, bot_panel == 2 ? 4 : bot_panel == 4 ? 12 : 6);
                const auto page = bot_buttons::page(current, bot_focus, bot_panel == 2 ? 2 : bot_panel == 1 ? 4 : 6);
                const int count = bot_panel == 4 ? (int)wrap_text(font, bot_alert, bot_geometry.width(864)).size() : bot_buttons::size(current);
                if (pressed & SCE_CTRL_UP) --bot_focus;
                if (pressed & SCE_CTRL_DOWN) ++bot_focus;
                if (pressed & SCE_CTRL_LEFT) bot_focus = bot_panel == 4 ? bot_focus - 6 : std::max(0, page.first - 1);
                if (pressed & SCE_CTRL_RIGHT) bot_focus = bot_panel == 4 ? bot_focus + 6 : std::min(count - 1, page.first + page.count);
                bot_focus = std::clamp(bot_focus, 0, std::max(0, count - 1));
                const bool sheet_ready = bot_panel != 1 || now - bot_sheet_since >= bot_buttons::sheet_duration;
                bool activate = sheet_ready && (pressed & SCE_CTRL_CROSS) && !(pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START));
                if (bot_panel == 4 && activate) { bot_panel = 0; activate = false; }
                if (tap) {
                    const auto touched_page = bot_buttons::page(current, bot_focus, bot_panel == 2 ? 2 : bot_panel == 1 ? 4 : 6);
                    int index = touched_page.first;
                    const auto sheet = bot_buttons::command_sheet(count, bot_focus, now - bot_sheet_since, false, bot_geometry.left);
                    const auto touched_cells = bot_panel == 1 ? sheet.cells() : bot_buttons::cells(current, bot_panel == 2 ? 358 : 142, 44, index, touched_page.count, bot_geometry.x(48), bot_geometry.x(912) - bot_geometry.x(48));
                    for (const auto& cell : touched_cells) {
                        if (sheet_ready && cell.hit(touch_x, touch_y)) { bot_focus = index; activate = true; }
                        ++index;
                    }
                    const int page_y = touch_y - (bot_panel == 1 ? sheet.offset : 0);
                    if (page_y >= 474 && page_y < 500) {
                        if ((bot_panel == 4 ? bot_focus / 6 > 0 : touched_page.first > 0) && bot_geometry.cell({0, 0, 200, 474, 165, 26}).hit(touch_x, page_y)) bot_focus = bot_panel == 4 ? std::max(0, bot_focus - 6) : std::max(0, touched_page.first - 1);
                        if ((bot_panel == 4 ? bot_focus / 6 * 6 + 6 < count : touched_page.first + touched_page.count < count) && bot_geometry.cell({0, 0, 390, 474, 165, 26}).hit(touch_x, page_y)) bot_focus = bot_panel == 4 ? std::min(count - 1, bot_focus + 6) : std::min(count - 1, touched_page.first + touched_page.count);
                    }
                    if (bot_panel == 1) {
                        if (sheet.back().hit(touch_x, touch_y) || touch_y < sheet.top + sheet.offset || touch_x < bot_geometry.left) close_bot_panel();
                    } else if (bot_geometry.cell({0, 0, 740, 502, 172, 40}).hit(touch_x, touch_y)) close_bot_panel();
                }
                if (activate && count && bot_panel && !(bot_panel == 1 && bot_sheet_closing)) {
                    const auto visible = bot_buttons::cells(current, 142, 44, bot_focus, 1);
                    if (!visible.empty()) {
                        if (tg.activateBotButton(bot_panel_message, visible.front().row, visible.front().column, bot_panel == 2)) { if (bot_panel != 2 || current.one_time) close_bot_panel(); }
                        else if (!tg.botStatus().empty()) { bot_alert = tg.botStatus(); open_bot_panel(4, 0); }
                    }
                }
                if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) close_bot_panel();
            }
            pressed = released = 0; tap = false;
        } else if (bot_chat) {
            int panel = 0; int64_t message = 0;
            if (tap && touch_y >= 498 && touch_y < 542) {
                if (bot_geometry.cell(bot_buttons::menu_control).hit(touch_x, touch_y) && !tg.botMenu().empty()) panel = 1;
                if (bot_geometry.cell(bot_buttons::keyboard_control).hit(touch_x, touch_y) && !tg.botKeyboard().buttons.empty()) {
                    panel = 2; message = tg.botKeyboard().id;
                }
            }
            if (tap && !panel) for (const auto& hit : bot_message_hits)
                if (touch_x >= hit.x && touch_x < hit.x + hit.w && touch_y >= hit.y && touch_y < hit.y + hit.h) {
                    webapp_diagnostics::record(webapp_diagnostics::Stage::InlineTap);
                    if (hit.row >= 0 && tg.activateBotButton(hit.message, hit.row, hit.column, false)) { tap = false; pressed = released = 0; }
                    else if (hit.row >= 0 && !tg.botStatus().empty()) { bot_alert = tg.botStatus(); panel = 4; }
                    else { panel = 3; message = hit.message; }
                    break;
                }
            if ((pressed & SCE_CTRL_SELECT) && !(history_view.combined && combined_list_focus)) {
                if (message_index >= 0 && message_index < (int)tg.messages().size() && !tg.messages()[message_index].buttons.empty()) {
                    panel = 3; message = tg.messages()[message_index].id;
                } else if (!tg.botMenu().empty()) panel = 1;
                else if (!tg.botKeyboard().buttons.empty()) { panel = 2; message = tg.botKeyboard().id; }
            }
            if (panel) { open_bot_panel(panel, message); tap = false; pressed = released = 0; }
        }
        const bool restore_webapp_touch = tg.botWebApp().phase != TelegramClient::BotWebApp::Closed && !touch_blocked;
        if (tg.botWebApp().phase != TelegramClient::BotWebApp::Closed && pending_web_link.empty()) {
#if VITAGRAM_WEBKIT
            using Phase = TelegramClient::BotWebApp;
            const auto phase = tg.botWebApp().phase;
            const bool editing_webapp = webapp_keyboard.active();
            if (phase == Phase::Confirm) {
                webkit_download.prepare();
                const auto engine_download = webkit_download.snapshot();
                if (!webapp_confirmed) webapp_diagnostics::record(webapp_diagnostics::Stage::ConfirmDrawn);
                webapp_confirmed = true;
                if ((pressed & SCE_CTRL_CROSS) || (tap && webapp_primary_rect.contains(touch_x, touch_y))) {
                    if (engine_download.state == WebKitDownload::State::Ready) {
                        auto* theme = webapp_theme(settings.isDark()); tg.openBotWebApp(theme); json_decref(theme);
                        webapp_error.clear(); webapp_main = webapp_back = false; webapp_main_text.clear();
                        webapp_header_fill = webapp_bottom_fill = webapp_background_fill = 0;
                    } else if (!engine_download.busy()) webkit_download.install();
                }
            } else if (phase == Phase::Ready && !webapp_view.active() && webapp_error.empty()) {
                photo_view.begin();
                if (!webapp_view.open(tg.botWebApp().signed_url, tg.botWebApp().same_origin,
                                      settings.proxy_enabled, settings.proxyConfig())) webapp_error = webapp_view.error();
            }
            if (phase == Phase::Ready && webapp_view.active()) {
                webapp_view.fontScale(settings.fontScale());
                if (!webapp_keyboard.active()) {
                    if (pressed & SCE_CTRL_TRIANGLE) webapp_fullscreen = !webapp_fullscreen;
                    const int previous_rotation = webapp_view.rotation();
                    const bool previous_fullscreen = webapp_view.fullscreen();
                    webapp_view.orientation(photo_view.rotation, webapp_fullscreen);
                    if (previous_rotation != webapp_view.rotation() || previous_fullscreen != webapp_view.fullscreen()) {
                        webapp_gesture.reset();
                        webapp_gesture.held = touching;
                        webapp_y = std::clamp(webapp_y + (previous_fullscreen ? 0 : 100) - (webapp_view.fullscreen() ? 0 : 100),
                            0.f, webapp_view.fullscreen() ? 543.f : 391.f);
                        webapp_view.move(webapp_x, webapp_y);
                        webapp_view.evaluate("window.Telegram && Telegram.WebView && Telegram.WebView.receiveEvent('viewport_changed',{height:window.innerHeight,is_state_stable:true,is_expanded:true})");
                    }
                }
                const bool fullscreen_webapp = webapp_view.fullscreen();
                const int webapp_top = fullscreen_webapp ? 0 : 100, webapp_height = fullscreen_webapp ? 544 : 392;
                const bool gesture_owned = webapp_gesture.captured;
                const bool gesture_cancelled = webapp_gesture.held && webapp_gesture.id < 0;
                const bool controller_webapp = webapp_stick_active ||
                    (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_CROSS | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER));
                const auto webapp_touch = webapp_gesture.update(!webapp_keyboard.active() && !controller_webapp, touch.reportNum,
                    touching ? touch.report[0].id : -1, touch_x, touch_y - webapp_top, 960, webapp_height, now);
                if (gesture_owned || gesture_cancelled || webapp_gesture.captured) tap = false;
                const int64_t pointer_elapsed = webapp_pointer_since ? now - webapp_pointer_since : 0;
                webapp_pointer_since = now;
                if (!webapp_keyboard.active()) {
                const float previous_webapp_x = webapp_x, previous_webapp_y = webapp_y;
                webapp_move_pointer(webapp_x, webapp_y, PhotoView::stickAxis(pad.lx), PhotoView::stickAxis(pad.ly), pointer_elapsed, webapp_height);
                if (pressed & SCE_CTRL_LEFT) webapp_x = std::max(0.f, webapp_x - 24);
                if (pressed & SCE_CTRL_RIGHT) webapp_x = std::min(959.f, webapp_x + 24);
                if (pressed & SCE_CTRL_UP) webapp_y = std::max(0.f, webapp_y - 24);
                if (pressed & SCE_CTRL_DOWN) webapp_y = std::min(float(webapp_height - 1), webapp_y + 24);
                if (controller_webapp) webapp_pointer_visible = true;
                if (int(previous_webapp_x) != int(webapp_x) || int(previous_webapp_y) != int(webapp_y))
                    webapp_view.move(webapp_x, webapp_y);
                if (pressed & SCE_CTRL_LTRIGGER) webapp_view.scroll(-180);
                if (pressed & SCE_CTRL_RTRIGGER) webapp_view.scroll(180);
                if (pressed & SCE_CTRL_CROSS) { webapp_view.click(webapp_x, webapp_y, true); webapp_view.click(webapp_x, webapp_y, false); }
                if (webapp_gesture.captured || webapp_touch.click) {
                    webapp_pointer_visible = false;
                    webapp_x = std::clamp(float(touch_x), 0.f, 959.f);
                    webapp_y = std::clamp(float(touch_y - webapp_top), 0.f, float(webapp_height - 1));
                    if (!webapp_gesture.dragged || webapp_touch.click) webapp_view.move(webapp_x, webapp_y);
                }
                if (webapp_touch.dx || webapp_touch.dy) {
                    const auto [dx, dy] = webapp_page_delta(webapp_view.rotation(), webapp_touch.dx, webapp_touch.dy);
                    webapp_view.scroll(dy, dx);
                }
                if (webapp_touch.click) {
                    webapp_view.click(webapp_x, webapp_y, true); webapp_view.click(webapp_x, webapp_y, false);
                }
                if ((pressed & SCE_CTRL_SELECT) && webapp_main && webapp_main_active) webapp_view.event("main_button_pressed");
                if (!fullscreen_webapp && tap && webapp_main && webapp_main_active && webapp_primary_rect.contains(touch_x, touch_y))
                    webapp_view.event("main_button_pressed");
                if (!fullscreen_webapp && tap && webapp_back && webapp_back_rect.contains(touch_x, touch_y))
                    webapp_view.event("back_button_pressed");
                if (pressed & SCE_CTRL_SQUARE) {
                    webapp_input_deadline = now + 5000000;
                    webapp_view.evaluate("(()=>{const e=document.activeElement;if(e&&(e.tagName==='INPUT'||e.tagName==='TEXTAREA')){window.__vitagramEditing=e;TelegramWebviewProxy.postEvent('vitagram_input',JSON.stringify({value:e.value,secret:e.type==='password'}));}})()");
                }
                }
                auto events = webapp_view.takeEvents();
                auto* batch = events.empty() ? nullptr : json_loads(events.c_str(), JSON_REJECT_DUPLICATES, nullptr);
                if (batch) {
                    if (json_is_true(json_object_get(batch, "overflow"))) webapp_error = tr("webapp.bridge_error");
                    size_t i; json_t* event;
                    json_array_foreach(json_object_get(batch, "events"), i, event) {
                        const auto* type = json_string_value(json_object_get(event, "type"));
                        const auto* raw = json_string_value(json_object_get(event, "data"));
                        if (!type || !raw) continue;
                        auto* data = json_loads(raw, JSON_REJECT_DUPLICATES, nullptr);
                        auto field = [&](const char* name) {
                            auto* item = json_object_get(data, name); const auto* value = json_string_value(item);
                            return value ? std::string(value, json_string_length(item)) : std::string{};
                        };
                        if (!std::strcmp(type, "vitagram_input")) {
                            if (webapp_input_deadline > now && !webapp_keyboard.active()) {
                                webapp_input_deadline = 0;
                                webapp_keyboard.configure(settings.custom_keyboard, settings.isDark(), settings.keyboard_languages, settings.keyboard_split);
                                webapp_keyboard.open(tr("webapp.input"), json_is_true(json_object_get(data, "secret")), field("value"));
                            }
                        } else if (!std::strcmp(type, "web_app_close")) {
                            tg.closeBotWebApp(); json_decref(data); break;
                        }
                        else if (!std::strcmp(type, "web_app_request_theme")) {
                            auto* theme = webapp_theme(settings.isDark());
                            auto params = webapp_theme_event(theme); webapp_view.event("theme_changed", params); json_decref(theme);
                        } else if (!std::strcmp(type, "web_app_set_header_color") || !std::strcmp(type, "web_app_set_bottom_bar_color") || !std::strcmp(type, "web_app_set_background_color")) {
                            unsigned color = 0;
                            const auto key = field("color_key");
                            if (key == "bg_color" || key == "secondary_bg_color" || key == "bottom_bar_bg_color") color = 0;
                            else if (!webapp_color(field("color"), color)) { webapp_diagnostics::recordUnsupported(type); webapp_error = tr("webapp.unsupported"); }
                            if (!std::strcmp(type, "web_app_set_header_color")) webapp_header_fill = color;
                            else if (!std::strcmp(type, "web_app_set_bottom_bar_color")) webapp_bottom_fill = color;
                            else webapp_background_fill = color;
                        } else if (!std::strcmp(type, "web_app_request_viewport") || !std::strcmp(type, "web_app_expand"))
                            webapp_view.evaluate("window.Telegram && Telegram.WebView && Telegram.WebView.receiveEvent('viewport_changed',{height:window.innerHeight,is_state_stable:true,is_expanded:true})");
                        else if (!std::strcmp(type, "web_app_request_safe_area"))
                            webapp_view.event("safe_area_changed", "{\"top\":0,\"bottom\":0,\"left\":0,\"right\":0}");
                        else if (!std::strcmp(type, "web_app_request_content_safe_area"))
                            webapp_view.event("content_safe_area_changed", "{\"top\":0,\"bottom\":0,\"left\":0,\"right\":0}");
                        else if (!std::strcmp(type, "web_app_setup_back_button")) webapp_back = json_is_true(json_object_get(data, "is_visible"));
                        else if (!std::strcmp(type, "web_app_setup_main_button")) {
                            webapp_main = json_is_true(json_object_get(data, "is_visible"));
                            webapp_main_active = json_is_true(json_object_get(data, "is_active")); webapp_main_text = field("text");
                        } else if (!std::strcmp(type, "web_app_setup_secondary_button") && !json_is_true(json_object_get(data, "is_visible"))) {
                            // The SDK resets hidden buttons when applying a new theme.
                        } else if (!std::strcmp(type, "web_app_data_send")) {
                            if (!tg.sendBotWebAppData(field("data"))) webapp_error = tr("webapp.data_error");
                        } else if (!std::strcmp(type, "web_app_open_link")) {
                            const auto address = field("url"); if (!webapp_origin(address).empty()) browser_error = open_link(address);
                        } else if (!std::strcmp(type, "web_app_open_tg_link")) {
                            const auto path = field("path_full");
                            if (!path.empty() && path[0] == '/' && path.find_first_of("\\\r\n") == std::string::npos) browser_error = open_link("https://t.me" + path);
                        } else if (!std::strcmp(type, "web_app_open_invoice")) {
                            const auto response = webapp_cancelled_invoice(data);
                            if (!response.empty()) webapp_view.event("invoice_closed", response);
                            webapp_diagnostics::recordUnsupported(type);
                            webapp_error = tr("webapp.payment_unavailable");
                        } else if (!std::strcmp(type, "web_app_open_popup")) webapp_view.event("popup_closed", "{\"button_id\":null}");
                        else if (!std::strcmp(type, "web_app_ready") || webapp_passive_event(type)) {} // No native action required.
                        else {
                            webapp_diagnostics::recordUnsupported(type);
                            if (webapp_error.empty()) webapp_error = tr("webapp.unsupported");
                        }
                        json_decref(data);
                    }
                    json_decref(batch);
                }
                const auto address = webapp_view.takeLink(); if (!address.empty()) browser_error = open_link(address);
                const auto old_theme = webapp_theme_json;
                auto* theme = webapp_theme(settings.isDark()); webapp_theme_json = webapp_theme_event(theme); json_decref(theme);
                if (old_theme != webapp_theme_json) webapp_view.event("theme_changed", webapp_theme_json);
                if (webapp_keyboard.active()) {
                    webapp_keyboard.handle(pressed, pad.buttons, touching, touch_x, touch_y, now, &touch);
                    if (webapp_keyboard.submitted()) {
                        auto* value = json_string(webapp_keyboard.text().c_str()); char* encoded = json_dumps(value, JSON_ENCODE_ANY);
                        if (encoded) webapp_view.evaluate("(()=>{const e=window.__vitagramEditing;if(e&&(e.tagName==='INPUT'||e.tagName==='TEXTAREA')){const p=e.tagName==='INPUT'?HTMLInputElement.prototype:HTMLTextAreaElement.prototype;Object.getOwnPropertyDescriptor(p,'value').set.call(e," + std::string(encoded) + ");e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}));}})()");
                        free(encoded); json_decref(value); webapp_keyboard.clear();
                    }
                }
                if (webapp_keyboard.cancelled()) webapp_keyboard.clear();
            }
            if (!webapp_keyboard.active() && !editing_webapp) {
                if ((pressed & SCE_CTRL_START) || (!webapp_view.fullscreen() && tap && webapp_close_rect.contains(touch_x, touch_y))) tg.closeBotWebApp();
                else if (pressed & SCE_CTRL_CIRCLE) {
                    if (webapp_back && phase == Phase::Ready) webapp_view.event("back_button_pressed"); else tg.closeBotWebApp();
                }
            }
#else
            tg.closeBotWebApp(); bot_alert = tr("bot.webapp_unavailable"); open_bot_panel(4, 0);
#endif
            pressed = released = 0; tap = false; touch_blocked = true; touch.reportNum = 0;
        }
#if VITAGRAM_WEBKIT
        if (tg.botWebApp().phase == TelegramClient::BotWebApp::Closed && (webapp_confirmed || webapp_view.active())) reset_webapp_view();
#endif
        const auto bot_link = tg.takeBotLink();
        if (!bot_link.empty()) browser_error = open_link(bot_link);

        if (!pending_web_link.empty()) {
            if (pressed & SCE_CTRL_LEFT) web_link_proceed = false;
            if (pressed & SCE_CTRL_RIGHT) web_link_proceed = true;
            repeat_scroll(ScrollContext::WebLink, 12);
            auto address_lines = wrap_text(font, pending_web_link, 700);
            if (pressed & SCE_CTRL_UP) --web_link_scroll;
            if (pressed & SCE_CTRL_DOWN) ++web_link_scroll;
            web_link_scroll = std::max(0, std::min(web_link_scroll, std::max(0, (int)address_lines.size()-12)));
            bool accept = (pressed & SCE_CTRL_CROSS) && web_link_proceed;
            bool cancel = (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) || ((pressed & SCE_CTRL_CROSS) && !web_link_proceed);
            if (tap) {
                if (touch_y >= 437 && touch_y < 477 && touch_x >= 500 && touch_x < 830) accept = true;
                else if ((touch_y >= 437 && touch_y < 477 && touch_x >= 130 && touch_x < 460) ||
                         touch_x < 100 || touch_x >= 860 || touch_y < 62 || touch_y >= 490) cancel = true;
            }
            if (accept) browser_error = open_web_link(pending_web_link);
            if (accept || cancel) pending_web_link.clear();
            pressed = 0; tap = false;
        }
        {
            const bool ui = (chat_ui_visible() || tool_menu_visible()) && !tg.historyAnchorPending();
            auto open_menu = [&]() {
                video_quality_menu = false;
                if (upload_picker.stage == UploadPicker::Stage::Menu) upload_picker.close();
                chat_menu_chat = tg.currentChatId();
                chat_menu_message = message_index >= 0 && message_index < (int)tg.messages().size() ? tg.messages()[message_index].id : 0;
                const bool check_delete = chat_menu_message && !photo_open && !video_fullscreen && !stickers_open && !gifs_open && !reactions_open &&
                    !circle_recorder.active() && !recorder.active() && !tg.topicsOpen();
                if (check_delete) tg.canDeleteMessage(chat_menu_message);
                chat_menu_waiting = check_delete && tg.deletePermissionPending(chat_menu_message);
                chat_menu_wait_since = now;
                chat_menu_open = !chat_menu_waiting; chat_menu_sel = 0; chat_menu_since = now;
                if (chat_menu_open) chat_menu_snapshot = chat_menu_items();
            };
            auto run_item = [&](const ChatMenuItem& item) {
                chat_menu_open = false;
                if (item.special == 1) { stickers_open = true; sticker_pack_menu = true; sticker_index = 0; sticker_pack = 1; tg.openStickers(); }
                else if (item.special == 2) { gifs_open = true; gif_index = 0; gif_next_page = -1; tg.searchGifs(); }
                else if (item.special == 3) {}   // already requested
                else if (item.special == 20) open_comments();
                else if (item.special == 10) tg.joinCurrentChat();
                else if (item.special == 30 || item.special == 31) {   // reply to / forward the selected message
                    const auto& list = tg.messages();
                    if (message_index >= 0 && message_index < (int)list.size()) {
                        if (item.special == 30) tg.replyTo(list[message_index].id);
                        else tg.beginForward(list[message_index].id);
                    }
                }
                else if (item.special == 32) tg.pasteForward();
                else if (item.special == 33) tg.cancelForward();
                else if (item.special == 34) {
                    if (message_index >= 0 && message_index < (int)tg.messages().size())
                        tg.deleteMessage(tg.messages()[message_index].id);
                }
                else if (item.special == 35) open_selected_set();
                else if (item.special == 36) tg.installStickerSet();
                else if (item.special == 40) open_seek();
                else if (item.special == 41 || item.special == 42) {
                    video_quality_menu = item.special == 41;
                    chat_menu_snapshot = chat_menu_items();
                    chat_menu_sel = 0; chat_menu_open = true;
                }
                else if (item.special >= 50) {
                    const int64_t position = post_animation().positionUs();
                    const bool playing = post_animation().playing();
                    if (tg.selectVideoQuality(message_index, item.special - 50)) {
                        quality_resume_chat = tg.currentChatId();
                        quality_resume_message = tg.messages()[message_index].id;
                        quality_resume_file = tg.messages()[message_index].animation_file_id;
                        quality_resume_time = position; quality_resume_playing = playing;
                        animation.setSource("");
                        fullscreen_seek = {}; seek_open = false; scrub_kind = 0;
                        raster_mismatch_chat = raster_mismatch_message = 0;
                        portrait_play_pending = portrait_preparation_requested = false;
                    }
                    video_quality_menu = false;
                }
                else if (item.special >= 11 && item.special <= 13) {
                    confirm_leave = true; confirm_age = 0; confirm_action = item.special == 11 ? 0 : item.special == 13 ? 1 : 2;
                }
                else if (item.special == 4) {   // go to the replied message
                    if (message_index >= 0 && message_index < (int)tg.messages().size()) tg.openPinnedMessage(tg.messages()[message_index].reply_to_id);
                }
                else pressed |= item.button;
            };
            const int64_t selected_menu_message = message_index >= 0 && message_index < (int)tg.messages().size() ? tg.messages()[message_index].id : 0;
            if (!ui || tg.currentChatId() != chat_menu_chat || selected_menu_message != chat_menu_message) {
                chat_menu_open = false; chat_menu_waiting = false;
            }
            if (chat_menu_waiting && ((pressed & SCE_CTRL_CIRCLE) ||
                (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_SELECT |
                    SCE_CTRL_CROSS | SCE_CTRL_SQUARE | SCE_CTRL_TRIANGLE | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)))) {
                chat_menu_waiting = false;
                pressed &= ~SCE_CTRL_CIRCLE;
            }
            if (chat_menu_waiting && tap && std::hypot(float(touch_x)-960.f, float(touch_y)-chat_corner_bottom) >= 66.f) chat_menu_waiting = false;
            if (chat_menu_waiting && (!tg.deletePermissionPending(chat_menu_message) || now - chat_menu_wait_since >= 2000000)) {
                chat_menu_waiting = false; chat_menu_open = true; chat_menu_sel = 0; chat_menu_since = now;
                chat_menu_snapshot = chat_menu_items();
            }
            if (ui && (tool_menu_visible() || !(history_view.combined && combined_list_focus)) && (tg.currentChatId() != 0 || tg.topicsOpen()) && (pressed & SCE_CTRL_START) && !(pad.buttons & SCE_CTRL_SELECT)) {
                if (chat_menu_open || chat_menu_waiting) { chat_menu_open = false; chat_menu_waiting = false; } else open_menu();
                pressed &= ~SCE_CTRL_START;
            } else if (ui && chat_menu_open) {
                repeat_scroll(ScrollContext::ChatMenu, std::min(history_menu_rows(), (int)chat_menu_snapshot.size()));
                const auto& items = chat_menu_snapshot;
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
                const bool in_right = std::hypot((float)touch_x - 960.0f, (float)touch_y - chat_corner_bottom) < 66.0f;
                const bool in_left = !settings.combined_view && std::hypot((float)touch_x, (float)touch_y - chat_corner_bottom) < 66.0f;
                if (in_right) {
                    if (chat_menu_open || chat_menu_waiting) { chat_menu_open = false; chat_menu_waiting = false; } else open_menu();
                    tap = false;
                } else if (in_left && tg.currentChatId() && tg.ready() && !photo_open && !tg.topicsOpen() && !circle_recorder.active() && !recorder.active()) {
                    chat_menu_open = false;
                    stickers_open = reactions_open = false;
                    if (gifs_open) { gifs_open = false; tg.closeGifs(); }
                    upload_picker.openMenu();
                    tap = false;
                } else if (chat_menu_open) {
                    const auto& items = chat_menu_snapshot;
                    const int first = menu_first((int)items.size(), chat_menu_sel);
                    const int body_top = kMenuBottom - std::min(history_menu_rows(), (int)items.size()) * kMenuRowHeight - 2 * kMenuPad, top = body_top + kMenuPad;
                    const bool inside = touch_x >= kMenuX && touch_x < kMenuX + kMenuWidth && touch_y >= body_top && touch_y < kMenuBottom;
                    if (inside && touch_y >= top && touch_y < kMenuBottom - kMenuPad) run_item(items[std::min((int)items.size() - 1, first + (touch_y - top) / kMenuRowHeight)]);
                    else if (!inside) chat_menu_open = false;
                    tap = false;
                }
            }
        }
        if (settings_open && theme_picker.active) {
            repeat_scroll(ScrollContext::ThemePicker, ThemePicker::visible);
            if (!font_pending && theme_touch_release && touch_dragged && theme_picker.hit(touch_start_x, touch_start_y) >= 0 &&
                std::abs(touch_y - touch_start_y) >= 30 && std::abs(touch_y - touch_start_y) > std::abs(touch_x - touch_start_x))
                theme_picker.scroll(touch_y < touch_start_y ? ThemePicker::visible - 1 : 1 - ThemePicker::visible);
            if (!font_pending && (pressed & SCE_CTRL_UP)) theme_picker.move(-1);
            if (!font_pending && (pressed & SCE_CTRL_DOWN)) theme_picker.move(1);
            if (tap) {
                const int row = theme_picker.hit(touch_x, touch_y);
                if ((font_picker || language_picker) && touch_y >= ThemePicker::top && touch_y < ThemePicker::top + ThemePicker::header &&
                    touch_x >= ThemePicker::left + ThemePicker::width - 170 && touch_x < ThemePicker::left + ThemePicker::width)
                    pressed |= SCE_CTRL_CIRCLE;
                else if (!font_pending && row >= 0) { theme_picker.focused = row; pressed |= SCE_CTRL_CROSS; }
                else if (!font_picker) theme_picker.close();
            }
            if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) {
                theme_picker.close(); font_pending = false;
                if (font_picker) rich_text_prepare_custom_font("");
            } else if (theme_picker.active && !font_pending && (pressed & SCE_CTRL_CROSS) && font_picker) {
                requested_font = theme_picker.focused == 0 ? "" : font_files[theme_picker.focused - 1];
                rich_text_prepare_custom_font(""); // Retry a file that may have been replaced after a failed read.
                rich_text_prepare_custom_font(requested_font);
                font_pending = true; settings_error.clear();
            } else if (theme_picker.active && language_picker && (pressed & SCE_CTRL_CROSS)) {
                requested_language = language_choice(theme_picker.focused);
                language_pending = true;
                rich_text_prepare_language_font(language_font_path_for(i18n::resolve(requested_language)));
                settings_error.clear();
                theme_picker.close();
            } else if (theme_picker.active && !font_picker && (pressed & SCE_CTRL_CROSS)) {
                const int previous = settings.theme;
                settings.theme = theme_picker.focused;
                if (settings.theme != previous && !settings.save()) {
                    settings.theme = previous;
                    settings.saveThemeHint();
                    settings_error = tr("settings.save_failed");
                } else settings_error.clear();
                settings_label_marquee.key = settings_value_marquee.key = -1;
                theme_picker.close();
            }
            tap = false;
            pressed = 0;
        }
        if (tap && !kb.active() && !confirm_leave && !spoiler_prompt && !unsupported_video_prompt && (recorder.active() || circle_recorder.active() || settings_open || !tg.canBrowse() || tg.inputKind() == InputKind::None)) {
            if (recorder.active()) {
                if (touch_y >= 444 && touch_y < 486) {
                    if (touch_x >= 50 && touch_x < 310 && recorder.state() != AudioRecordingState::Encoding) pressed |= SCE_CTRL_CROSS;
                    else if (touch_x >= 350 && touch_x < 610 && recorder.state() == AudioRecordingState::Review) pressed |= SCE_CTRL_SQUARE;
                    else if (touch_x >= 650 && touch_x < 910) pressed |= SCE_CTRL_CIRCLE;
                }
            } else if (circle_recorder.active()) {
                // Recorder actions live in the corner menu now; the bottom strip is not a touch target.
            } else if (qr_scanner.active()) {
                if (touch_y >= 500) pressed |= SCE_CTRL_CIRCLE;
            } else if (!settings_open && !chat_modal_open() && tg.topicsOpen() && (!history_view.combined || history_view.historyHit(touch_x, touch_y, 544))) {
                combined_list_focus = false;
                if (history_view.topicHit(touch_x, touch_y) >= 0) {
                    int index = std::max(0, topic_index-history_view.topicRows()+1)+history_view.topicHit(touch_x, touch_y);
                    if (index < (int)tg.topics().size()) { topic_index = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (upload_picker.active()) {
                if (upload_picker.stage == UploadPicker::Stage::Menu) {
                    const int index = history_view.attachHit(touch_x, touch_y, (int)attach_menu_items().size());
                    if (index >= 0) {
                        upload_picker.selected = index; pressed |= SCE_CTRL_CROSS;
                    } else pressed |= SCE_CTRL_CIRCLE;
                } else if (upload_picker.stage == UploadPicker::Stage::Review && touch_x >= 540 && touch_y >= 109 && touch_y < 465) {
                    const auto* focus = upload_picker.focusedItem();
                    if (focus && focus->kind == UploadPicker::Kind::Video) pressed |= SCE_CTRL_CROSS;
                } else if (touch_y >= 500) {
                    if (upload_picker.stage == UploadPicker::Stage::Browse) {
                        pressed |= touch_x < 250 ? SCE_CTRL_SQUARE : touch_x < 500 ? SCE_CTRL_TRIANGLE : SCE_CTRL_CIRCLE;
                    } else pressed |= touch_x < 240 ? SCE_CTRL_CROSS : touch_x < 480 ? SCE_CTRL_SQUARE :
                        touch_x < 720 ? SCE_CTRL_TRIANGLE : SCE_CTRL_CIRCLE;
                } else if (upload_picker.stage == UploadPicker::Stage::Review && touch_y >= 117 && touch_y < 461 && touch_x < 530) {
                    int index = (upload_picker.selected / 8) * 8 + (touch_y - 117) / 43;
                    if (index < upload_picker.reviewRows()) { upload_picker.selected = index; pressed |= SCE_CTRL_CROSS; }
                } else if (upload_picker.stage == UploadPicker::Stage::Browse && touch_y >= 117 && touch_y < 461) {
                    int index = (upload_picker.selected / 8) * 8 + (touch_y - 117) / 43;
                    if (index < (int)upload_picker.entries.size()) { upload_picker.selected = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (!settings_open && gifs_open) {
                if (touch_y >= 70 && touch_y < 110) pressed |= SCE_CTRL_SQUARE;
                else if (touch_y >= 130 && touch_y < 448 && touch_x >= 30 && touch_x < 930) {
                    const int index = gif_index / 12 * 12 + (touch_y - 130) / 106 * 4 + (touch_x - 30) / 225;
                    if (index < (int)tg.gifs().items.size()) { gif_index = index; pressed |= SCE_CTRL_CROSS; }
                } else if (touch_y >= 455 && touch_y < 495) {
                    if (touch_x < 320) pressed |= SCE_CTRL_LTRIGGER;
                    else if (touch_x >= 640) pressed |= SCE_CTRL_RTRIGGER;
                    else pressed |= SCE_CTRL_TRIANGLE;
                }
            } else if (!settings_open && stickers_open) {
                if (touch_y >= 500) {}
                else if (sticker_pack_menu && touch_y >= 116 && touch_y < 494) {
                    int index = std::max(0, sticker_pack - 6) + (touch_y - 116) / 54;
                    if (index < (int)tg.stickerPacks().size()) { sticker_pack = index; pressed |= SCE_CTRL_CROSS; }
                } else if (!sticker_pack_menu && tg.stickerSetOpen() && tg.stickerSetCanInstall() && touch_y >= 470 && touch_y < 508) pressed |= SCE_CTRL_TRIANGLE;
                else if (!sticker_pack_menu && touch_x >= 30 && touch_x < 930 && touch_y >= 130 && touch_y < 460) {
                    int index = (sticker_index/18)*18 + (touch_y-130)/110*6 + (touch_x-30)/150;
                    if (index < (int)tg.stickers().size()) { sticker_index = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (!settings_open && reactions_open) {
                if (touch_x >= 32 && touch_x < 232 && touch_y >= 444 && touch_y < 488) pressed |= SCE_CTRL_CIRCLE;
                else {
                    const ReactionGrid grid{(int)tg.reactionChoices().size(), tg.reactionTopCount()};
                    const int index = grid.hit(touch_x, touch_y, reaction_index);
                    if (index >= 0) { reaction_index = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (video_fullscreen) {
                pressed |= SCE_CTRL_CROSS;
            } else if (!settings_open && post_open && !photo_open && message_index >= 0 && message_index < (int)tg.messages().size() && tg.messages()[message_index].poll.id && (!history_view.combined || history_view.historyHit(touch_x, touch_y, 544))) {
                combined_list_focus = false;
                if (history_view.pollHit(touch_x, touch_y) >= 0) {
                    int index = poll_index / 5 * 5 + history_view.pollHit(touch_x, touch_y);
                    if (index < (int)tg.messages()[message_index].poll.order.size()) { poll_index = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (settings_open) {
                if (settings_menu::backHit(touch_x, touch_y)) pressed |= SCE_CTRL_CIRCLE;
                else if (folder_picker.active) {
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
                else if (about_open) {
                    const int link = about_layout(font).hit_link(touch_x, touch_y, about_offset);
                    if (link >= 0) { about_link = link; pressed |= SCE_CTRL_CROSS; }
                } else if (logs_open) {
                    if (touch_y >= 104 && touch_y < 104 + 36 * diag_log::kCount) {
                        settings_row = (touch_y - 104) / 36;
                        pressed |= SCE_CTRL_CROSS;
                    }
                } else if (proxy_open) {
                    if (touch_y >= 104 && touch_y < 104 + 36 * (int)proxy_rows(proxy_draft.proxy_type).size()) {
                        settings_row = (touch_y - 104) / 36;
                        pressed |= SCE_CTRL_CROSS;
                    }
                } else if (touch_y >= kSettingsTop && touch_y < kSettingsTop + kSettingsViewport) {
                    const int hit = settings_scroll.hit(touch_y - kSettingsTop);
                    if (hit >= 0 && settings_menu::rowHit(touch_x, touch_y, kSettingsTop + hit * kSettingsRowHeight - settings_scroll.offset(), kSettingsRowHeight)) { settings_row = hit; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (!tg.currentChatId() && !tg.canBrowse()) {
                if ((touch_x >= 30 && touch_x < 470 && touch_y >= 280 && touch_y < 340 && tg.qrLink().empty()) ||
                    (touch_y >= 500 && touch_x >= 480 && tg.qrLink().empty())) pressed |= SCE_CTRL_START;
                else if (tg.canRequestQr() && touch_x >= 510 && touch_x < 930 && touch_y >= 280 && touch_y < 340)
                    pressed |= SCE_CTRL_SQUARE;
                else if (!tg.qrLink().empty() && touch_y >= 500) pressed |= SCE_CTRL_CIRCLE;
                else if (touch_y >= 500 && touch_x < 480) pressed |= SCE_CTRL_CROSS;
            } else if (!chat_modal_open() && history_view.combined && history_view.searchHit(touch_x, touch_y)) {
                combined_list_focus = true; search_bar_focus = true; pressed |= SCE_CTRL_SQUARE;
            } else if (!chat_modal_open() && history_view.combined && touch_y < list_bottom &&
                       hit_chat(touch_x, touch_y) >= 0) {
                const int index = hit_chat(touch_x, touch_y);
                if (index >= 0 && index < (int)tg.chats().size()) {
                    chat_index = index; combined_list_focus = true; search_bar_focus = false; pressed |= SCE_CTRL_CROSS;
                }
            } else if ((!tg.currentChatId() || history_view.combined) && tg.canBrowse() && history_view.folderBandHit(touch_x, touch_y)) {
                int index = (touch_x - 10) / kFolderStep + folder_scroll / kFolderStep;
                if ((touch_x - 10) % kFolderStep >= ui_layout::folder_width) index = -1;
                if (index >= 0 && index < (int)tg.folders().size()) { tg.selectFolder(tg.folders()[index].id); chat_index = 0; chat_scroll.reset(); combined_list_focus = true; }
            } else if (!tg.currentChatId() && tg.canBrowse() && touch_y >= 42 && touch_y < 60) {
                tg.loadMoreChats();
            } else if (tg.currentChatId() && tg.ready() && !tg.historyAnchorPending() && !post_open && !photo_open && !tg.topicsOpen() &&
                       !settings.combined_view && std::hypot((float)touch_x, (float)touch_y - chat_corner_bottom) < 66.0f) {
                upload_picker.openMenu(); chat_menu_open = false;
            } else if (touch_y >= 500 && !tg.currentChatId() && !tg.canBrowse()) {
                pressed |= touch_x >= 650 ? SCE_CTRL_TRIANGLE : SCE_CTRL_CROSS;
            } else if (tg.currentChatId() && !post_open && !photo_open && history_view.historyHit(touch_x, touch_y, history_bottom)) {
                combined_list_focus = false;
                bool voice_tapped = false;
                for (const auto& quote : reply_rects)
                    if (quote.second.contains(touch_x, touch_y)) {
                        for (const auto& message : tg.messages()) if (message.id == quote.first) {
                            if (!message.reply_chat_id || message.reply_chat_id == tg.currentChatId())
                                tg.openPinnedMessage(message.reply_to_id);
                            break;
                        }
                        voice_tapped = true;
                        break;
                    }
                if (!voice_tapped)
                    for (const auto& button : voice_play_rects)
                        if (button.second.contains(touch_x, touch_y)) {
                            for (size_t k = 0; k < tg.messages().size(); ++k) if (tg.messages()[k].id == button.first) message_index = (int)k;
                            history_voice_id = button.first; history_voice_toggle = true; voice_tapped = true;
                            break;
                        }
                if (!voice_tapped)
                    for (const auto& button : comments_rects)
                        if (button.second.contains(touch_x, touch_y)) {
                            for (size_t k = 0; k < tg.messages().size(); ++k) if (tg.messages()[k].id == button.first) message_index = (int)k;
                            voice_tapped = open_comments(); break;
                        }
                if (!voice_tapped)
                    for (const auto& row : history_layout(font, tg, message_index))
                        if (touch_y >= row.top && touch_y < row.top + row.height - row.gap()) {
                            message_index = row.index; pressed |= SCE_CTRL_CROSS; break;
                        }
            } else if (post_open && !photo_open && message_index >= 0 && message_index < (int)tg.messages().size()) {
                const auto& post = tg.messages()[message_index];
                if (history_view.historyHit(touch_x, touch_y, 544)) {
                    combined_list_focus = false;
                    if (post.voice && touch_x >= history_view.x(160) && touch_x < 800 &&
                        touch_y >= history_view.y(90)-post_text_scroll && touch_y < history_view.y(300)-post_text_scroll) {
                        pressed |= SCE_CTRL_CROSS;
                    } else if (post.video && touch_x >= history_view.x(30) && touch_x < 930 &&
                        touch_y >= post_media_top()-post_text_scroll &&
                        touch_y < post_media_top()+post_media_height_for(post)-post_text_scroll) {
                        pressed |= SCE_CTRL_CROSS;
                    } else if (!post.photos.empty() && touch_y >= post_photo_top()-post_text_scroll && touch_y < post_photo_top()+170-post_text_scroll) {
                        for (const auto& tile : photo_carousel(post, photo_index))
                            if (touch_x >= tile.x && touch_x < tile.x+tile.width) {
                                photo_index = tile.index; pressed |= SCE_CTRL_CROSS; break;
                            }
                    } else {
                        auto lines = post_lines(font, post);
                        const int preview_top = post_preview_top(post, lines.size()) - post_text_scroll;
                        int relative = touch_y - (post_text_top(post)-19-post_text_scroll);
                        int line = relative / post_pitch();
                        if (!post.preview_url.empty() && touch_x >= history_view.x(38) && touch_x < 922 &&
                            touch_y >= preview_top && touch_y < preview_top + link_preview_height(post))
                            browser_error = open_link(post.preview_url);
                        else if (relative >= 0 && line < (int)lines.size() && lines[line].comments) open_comments();
                        else if (relative >= 0 && line < (int)lines.size() && !spoiler_visibility.revealed(tg.currentChatId(), post.id) &&
                            std::any_of(lines[line].styles.begin(), lines[line].styles.end(), [](const StyleSpan& span) { return span.style & TextSpoiler; })) pressed |= SCE_CTRL_CROSS;
                        else if (relative >= 0 && line < (int)lines.size() && !lines[line].url.empty())
                            browser_error = open_link(post_link_at(font, lines[line], touch_x, history_view.x(38)));
                    }
                }
            } else if ((!history_view.combined || search_open) && !tg.currentChatId() && tg.canBrowse() && touch_y >= 66 && touch_y < kSearchTop - 3 && (search_open || touch_y < 71 + kSearchBarShift - 3)) {
                pressed |= SCE_CTRL_SQUARE;   // the search field
            } else if (search_open && !tg.currentChatId() && tg.canBrowse()) {
                const int index = search_first + (touch_y - kSearchTop) / kSearchRow;
                if (touch_y >= kSearchTop && index >= 0 && index < (int)tg.searchResults().size() && index < search_first + kSearchRows) {
                    search_index = index; pressed |= SCE_CTRL_CROSS;
                }
            } else if (!tg.currentChatId() && tg.canBrowse() && touch_y < list_bottom && hit_chat(touch_x, touch_y) >= 0) {
                int index = hit_chat(touch_x, touch_y);
                if (index >= 0 && index < (int)tg.chats().size()) { chat_index = index; search_bar_focus = false; pressed |= SCE_CTRL_CROSS; }
            }
        }

        if (tg.historyAnchorPending()) message_index = -1;
        if (tg.currentChatId() != 0 && !tg.historyAnchorPending() && !tg.messages().empty()) {
            if (message_index < 0 && !tg.historyAnchorPending()) message_index = (int)tg.messages().size() - 1;
            message_index = std::max(0, std::min(message_index < 0 ? 0 : message_index, (int)tg.messages().size() - 1));
            photo_index = std::max(0, std::min(photo_index, (int)tg.messages()[message_index].photos.size() - 1));
        }

        if (confirm_leave) {
            if (!tg.currentChatId() || !tg.ready()) confirm_leave = false;
            else if (confirm_age++ > 0) {   // the frame that opened the dialog may still carry its tap
                bool leave = pressed & SCE_CTRL_CROSS, cancel = pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START);
                if (tap) { if (touch_y >= 300 && touch_y < 344) leave = true; else cancel = true; }   // the second button cancels, like a tap outside
                if (leave) { if (confirm_action == 0) tg.leaveCurrentGroup(); else tg.deleteCurrentHistory(confirm_action == 1); }
                if (leave || cancel) confirm_leave = false;
            }
            pressed = 0; tap = false;
        }
        if (!circle_recorder.active() && !recorder.active() && !settings_open && !(pressed & SCE_CTRL_START) && tg.inputKind() != InputKind::None && (tg.inputKind() == InputKind::Message || (pressed & SCE_CTRL_CROSS)) && !kb.active() && !kb.submitted() && !kb.cancelled()) {
            kb.open(tg.inputPrompt(), tg.inputSecret(), "", tg.inputKind() == InputKind::Message);
        }

        // Sign-in: step back to the phone number, or ask Telegram for the code again.
        if (!circle_recorder.active() && !recorder.active() && !settings_open && !kb.active() && !tg.canBrowse()) {
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

        if (history_view.combined && !chat_modal_open() && !chat_menu_open && !bot_panel &&
            !confirm_leave && !spoiler_prompt && pending_web_link.empty()) {
            if ((tg.currentChatId() || tg.topicsOpen()) && !post_open && !combined_list_focus && (pressed & SCE_CTRL_LEFT)) {
                combined_list_focus = true; pressed &= ~SCE_CTRL_LEFT;
            } else if ((tg.currentChatId() || tg.topicsOpen()) && combined_list_focus && (pressed & SCE_CTRL_RIGHT)) {
                combined_list_focus = false; pressed &= ~SCE_CTRL_RIGHT;
            }
        }
        bool toggle_voice = false;
        bool toggle_video = false;
        bool record_toggle = false;

        if (spoiler_prompt) {
            bool accept = pressed & SCE_CTRL_CROSS;
            bool cancel = pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE);
            if (tap) {
                if (touch_x >= 125 && touch_x < 460 && touch_y >= 330 && touch_y < 380) accept = true;
                else cancel = true;
            }
            const auto& shown = tg.messages();
            if (tg.currentChatId() != spoiler_prompt_chat || message_index < 0 || message_index >= (int)shown.size() ||
                shown[message_index].id != spoiler_prompt_message) { accept = false; cancel = true; }
            if (accept) {
                reveal_spoiler(shown[message_index]);
                spoiler_prompt = false;
                if (shown[message_index].spoiler && !shown[message_index].photos.empty()) {
                    tg.ensureFullPhoto((size_t)message_index, (size_t)photo_index);
                    photo_open = true; photo_view.begin();
                } else if (shown[message_index].spoiler && shown[message_index].video) toggle_video = true;
            } else if (cancel) spoiler_prompt = false;
            pressed = 0; tap = false;
        }
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
        if ((pressed & SCE_CTRL_START) && !(pad.buttons & SCE_CTRL_SELECT) && !chat_modal_open() && !kb.active() && !qr_scanner.active() && !circle_recorder.active() && !recorder.active() && (!tg.currentChatId() || (settings.combined_view && combined_list_focus))) {
            bool auth_proxy = !settings_open && !tg.canBrowse();
            settings_open = !settings_open;
            proxy_open = auth_proxy;
            logs_open = false;
            settings_section = 0;
            keyboard_settings = 0;
            about_open = false;
            about_chat_pending = false;
            proxy_return_to_screen = auth_proxy;
            proxy_imported = false;
            if (auth_proxy) proxy_draft = settings;
            settings_error.clear();
            settings_label_marquee.key = settings_value_marquee.key = -1;
            settings_row = 0; settings_scroll.reset(); confirm_logout = false; confirm_vault_off = false; pressed = 0;
        }
        if (kb.active()) {
            kb.handle(pressed, pad.buttons, touching, touch_x, touch_y, now, &touch);
        } else if (kb.submitted()) {
            std::string v = kb.text();
            kb.close();
            if (upload_text_input) {
                upload_text_input = false;
                if (upload_picker.stage == UploadPicker::Stage::Review) upload_picker.caption = v;
            } else if (gif_input) {
                gif_input = false; gif_index = 0; gif_next_page = -1;
                if (gifs_open && tg.gifsCurrent()) tg.searchGifs(v);
            } else if (search_input) {
                search_input = false; search_index = 0;
                tg.searchChats(v);
            } else if (folder_name_input) {
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
                    else settings_error = i18n::text("proxy.err.port_range");
                }
                proxy_input = -1;
            } else tg.submitInput(v);
        } else if (kb.cancelled()) {
            kb.close();
            if (upload_text_input) upload_text_input = false;
            else if (gif_input) gif_input = false;
            else if (search_input) { search_input = false; if (tg.searchQuery().empty()) search_open = false; }
            else if (folder_name_input) folder_name_input = false;
            else if (proxy_input >= 0) proxy_input = -1; else tg.cancelInput();
        } else if (recorder.active()) {
            if (pressed & SCE_CTRL_CIRCLE) { recorder.cancel(); audio_error.clear(); }
            else {
                const auto state = recorder.state();
                if ((pressed & SCE_CTRL_SQUARE) && state == AudioRecordingState::Review) recorder.togglePreview();
                if (pressed & SCE_CTRL_CROSS) {
                    audio_error.clear();
                    if (state == AudioRecordingState::Preview) recorder.record();
                    else if (state == AudioRecordingState::Recording) recorder.finish();
                    else if (state == AudioRecordingState::Review) {
                        if (tg.sendAudioFile(recorder.path(), recorder.seconds(), audio_chat, audio_topic)) recorder.releaseFile();
                        else audio_error = i18n::text("rec.send_failed");
                    } else if (state == AudioRecordingState::Error) open_audio();
                }
            }
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
        } else if (qr_scanner.active()) {
            if (pressed & SCE_CTRL_CIRCLE) { qr_scanner.stop(); settings_row = proxy_scan_row; }
        } else if (!settings_open && !chat_modal_open() && tg.topicsOpen() && !(history_view.combined && combined_list_focus)) {
            if (pressed & SCE_CTRL_CIRCLE) tg.leaveChat();
            else {
                repeat_scroll(ScrollContext::Topics, history_view.topicRows());
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
            const int rows = upload_picker.stage == Stage::Menu ? (int)attach_menu_items().size() : upload_picker.stage == Stage::Review ? upload_picker.reviewRows() : (int)upload_picker.entries.size();
            if (rows > 0) {
                const auto context = upload_picker.stage == Stage::Menu ? ScrollContext::AttachMenu :
                    upload_picker.stage == Stage::Browse ? ScrollContext::UploadBrowse : ScrollContext::UploadReview;
                repeat_scroll(context, upload_picker.stage == Stage::Menu ? rows : 8);
                if (pressed & SCE_CTRL_UP) upload_picker.selected = (upload_picker.selected + rows - 1) % rows;
                if (pressed & SCE_CTRL_DOWN) upload_picker.selected = (upload_picker.selected + 1) % rows;
                if (upload_picker.stage == Stage::Browse) {
                    if (pressed & SCE_CTRL_LTRIGGER) upload_picker.selected = std::max(0, upload_picker.selected - 8);
                    if (pressed & SCE_CTRL_RTRIGGER) upload_picker.selected = std::min(rows - 1, upload_picker.selected + 8);
                }
            }
            if (upload_picker.stage == Stage::Menu) {
                if (settings.recording_shortcuts && (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
                    upload_picker.close();
                    if (pressed & SCE_CTRL_LTRIGGER) open_circle(); else open_audio();
                }
                else if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) upload_picker.close();
                else if (pressed & SCE_CTRL_CROSS) {
                    const auto items = attach_menu_items();
                    const int action = items[std::min(upload_picker.selected, (int)items.size()-1)].special;
                    if (action < 2) upload_picker.chooseKind(action == 0 ? UploadPicker::Kind::Media : UploadPicker::Kind::File);
                    else {
                        upload_picker.close();
                        if (action == 3) open_circle();
                        else if (action == 4) record_toggle = true;
                        else if (action == 5) { gifs_open = true; gif_index = 0; gif_next_page = -1; tg.searchGifs(); }
                        else if (action == 6) { stickers_open = true; sticker_pack_menu = true; sticker_index = 0; sticker_pack = 1; tg.openStickers(); }
                        else if (action == 7 && message_index >= 0 && message_index < (int)tg.messages().size()) {
                            reactions_open = true; reaction_index = 0;
                            tg.loadReactions(tg.messages()[message_index].id, true);
                        }
                    }
                }
            } else if (upload_picker.stage == Stage::Browse) {
                if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))
                    upload_picker.changeSort((pressed & SCE_CTRL_LEFT) ? -1 : 1);
                if (pressed & SCE_CTRL_CIRCLE) upload_picker.up();
                else if (pressed & SCE_CTRL_TRIANGLE) upload_picker.toggleHidden();
                else if (pressed & SCE_CTRL_SQUARE) upload_picker.finishSelection();
                else if (pressed & SCE_CTRL_CROSS) upload_picker.enter();
            } else {
                const auto* focus = upload_picker.focusedItem();
                if (pressed & SCE_CTRL_CIRCLE) upload_picker.close();
                else if (pressed & SCE_CTRL_TRIANGLE) upload_picker.removeSelected();
                else if (pressed & SCE_CTRL_SQUARE) upload_picker.sendBatch();
                else if (pressed & SCE_CTRL_CROSS) {
                    if (upload_picker.textSelected()) {
                        upload_text_input = true;
                        kb.open(tr(upload_picker.captionKey()), false, upload_picker.caption, true);
                    } else if (focus && focus->kind == UploadPicker::Kind::Video) toggle_video = true;
                    else upload_picker.reviewActivate();
                }
            }
        } else if (!settings_open && gifs_open) {
            const int count = (int)tg.gifs().items.size();
            if (pressed & SCE_CTRL_CIRCLE) { gifs_open = false; tg.closeGifs(); }
            else if (pressed & SCE_CTRL_SQUARE) {
                gif_input = true; kb.open(tr("gif.kb_title"), false, tg.gifs().query);
            } else if (pressed & SCE_CTRL_TRIANGLE) { gif_index = 0; gif_next_page = -1; tg.searchGifs(); }
            else {
                if (pressed & SCE_CTRL_LEFT) --gif_index;
                if (pressed & SCE_CTRL_RIGHT) ++gif_index;
                repeat_scroll(ScrollContext::Gifs, 3);
                if (pressed & SCE_CTRL_UP) gif_index -= 4;
                if (pressed & SCE_CTRL_DOWN) gif_index += 4;
                if (pressed & SCE_CTRL_LTRIGGER) gif_index = std::max(0, gif_index / 12 - 1) * 12;
                if (pressed & SCE_CTRL_RTRIGGER) gif_index = (gif_index / 12 + 1) * 12;
                if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) gif_next_page = -1;
                if (gif_index >= count && !tg.gifs().end && !tg.gifs().query.empty()) {
                    gif_next_page = gif_index;
                    if (!tg.gifs().pending) tg.loadMoreGifs();
                }
                gif_index = std::max(0, std::min(std::max(0, count - 1), gif_index));
                if ((pressed & SCE_CTRL_CROSS) && tg.sendGif(gif_index)) { gifs_open = false; tg.closeGifs(); }
            }
        } else if (!settings_open && stickers_open) {
            if (sticker_pack_menu) {
                if (pressed & SCE_CTRL_CIRCLE) stickers_open = false;
                int count = (int)tg.stickerPacks().size();
                repeat_scroll(ScrollContext::StickerPacks, 7);
                if (pressed & SCE_CTRL_UP) --sticker_pack;
                if (pressed & SCE_CTRL_DOWN) ++sticker_pack;
                sticker_pack = std::max(0, std::min(std::max(0, count - 1), sticker_pack));
                if ((pressed & SCE_CTRL_CROSS) && count) {
                    sticker_pack_menu = false; sticker_index = 0; tg.selectStickerPack(sticker_pack);
                }
            } else {
                if (pressed & SCE_CTRL_SQUARE) stickers_open = false;
                if (pressed & SCE_CTRL_CIRCLE) { sticker_pack_menu = true; tg.openStickers(); }
                if ((pressed & SCE_CTRL_TRIANGLE) && tg.stickerSetCanInstall() && tg.stickerSetOpen()) tg.installStickerSet();
                int count = (int)tg.stickers().size();
                if (pressed & SCE_CTRL_LEFT) --sticker_index;
                if (pressed & SCE_CTRL_RIGHT) ++sticker_index;
                repeat_scroll(ScrollContext::Stickers, 3);
                if (pressed & SCE_CTRL_UP) sticker_index -= 6;
                if (pressed & SCE_CTRL_DOWN) sticker_index += 6;
                sticker_index = std::max(0, std::min(std::max(0, count-1), sticker_index));
                if ((pressed & SCE_CTRL_CROSS) && tg.sendSticker(sticker_index)) stickers_open = false;
            }
        } else if (!settings_open && reactions_open) {
            int count = (int)tg.reactionChoices().size();
            if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_SQUARE)) reactions_open = false;
            if (count) {
                const ReactionGrid grid{count, tg.reactionTopCount()};
                if (pressed & SCE_CTRL_LEFT) reaction_index = grid.move(reaction_index, -1);
                if (pressed & SCE_CTRL_RIGHT) reaction_index = grid.move(reaction_index, 1);
                repeat_scroll(ScrollContext::Reactions, 4);
                if (pressed & SCE_CTRL_UP) reaction_index = grid.move(reaction_index, -8);
                if (pressed & SCE_CTRL_DOWN) reaction_index = grid.move(reaction_index, 8);
                reaction_index = std::max(0, std::min(count-1, reaction_index));
                if ((pressed & SCE_CTRL_CROSS) && message_index >= 0 && message_index < (int)tg.messages().size())
                    tg.toggleReaction(tg.messages()[message_index].id, tg.reactionChoices()[reaction_index]);
            }
        } else if (settings_open && folder_picker.active) {
            repeat_scroll(ScrollContext::Folders, 8);
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
                settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                folder_picker.active = false;
            }
        } else if (settings_open && about_open) {
            const auto& layout = about_layout(font);
            repeat_scroll(ScrollContext::About, 414.0 / 28);
            if (pressed & SCE_CTRL_UP) about_offset -= 28;
            if (pressed & SCE_CTRL_DOWN) about_offset += 28;
            if (pressed & SCE_CTRL_LTRIGGER) about_offset -= 386;
            if (pressed & SCE_CTRL_RTRIGGER) about_offset += 386;
            if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
                about_link = about_link < 0 ? ((pressed & SCE_CTRL_LEFT) ? 1 : 0) : 1 - about_link;
                const int y = layout.link_y(about_link);
                if (y < about_offset) about_offset = y;
                else if (y + 60 > about_offset + 414) about_offset = y + 60 - 414;
            }
            about_offset = std::clamp(about_offset, 0, layout.maximum());
            if (pressed & SCE_CTRL_CIRCLE) {
                about_open = false; about_chat_pending = false; restore_settings_parent();
            } else if ((pressed & SCE_CTRL_CROSS) && about_link >= 0 && layout.link_visible(about_link, about_offset)) {
                if (about_link == 0) browser_error = open_link(kAboutProjectUrl);
                else if (!tg.ready()) settings_error = i18n::text("about.telegram_unavailable");
                else { about_chat_pending = true; browser_error = open_link(kAboutAuthorUrl); settings_error.clear(); }
            }
        } else if (settings_open && keyboard_settings) {
            if (pressed & SCE_CTRL_CIRCLE) {
                if (keyboard_settings == 2) { keyboard_settings = 1; settings_row = 1; }
                else { keyboard_settings = 0; restore_settings_parent(); }
                settings_scroll.configure(keyboard_settings ? keyboard_rows() : settings_menu::counts[settings_section], kSettingsRowHeight, kSettingsViewport);
                if (keyboard_settings) settings_scroll.reset();
                settings_scroll.reveal(settings_row); settings_error.clear();
            } else {
                const int count = keyboard_rows();
                repeat_scroll(ScrollContext::KeyboardSettings, std::min(count, kSettingsViewport / kSettingsRowHeight));
                if (pressed & SCE_CTRL_UP) settings_row = (settings_row + count - 1) % count;
                if (pressed & SCE_CTRL_DOWN) settings_row = (settings_row + 1) % count;
                settings_scroll.reveal(settings_row);
                if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) {
                    bool changed = false;
                    if (keyboard_settings == 1 && settings_row == 0) {
                        settings.custom_keyboard = !settings.custom_keyboard; changed = true;
                    } else if (keyboard_settings == 1 && settings_row == 2) {
                        settings.keyboard_split = (settings.keyboard_split + ((pressed & SCE_CTRL_LEFT) ? 2 : 1)) % 3;
                        changed = true;
                    } else if (keyboard_settings == 1) {
                        if (pressed & SCE_CTRL_CROSS) {
                            keyboard_settings = 2; settings_row = 0; settings_scroll.reset(); settings_error.clear();
                        }
                    } else if (settings_row == 0) {
                        settings.keyboard_languages.clear(); changed = true;
                    } else {
                        changed = keyboard_languages::toggle(settings.keyboard_languages,
                            keyboard_layout::all()[settings_row - 1].code, i18n::systemLanguage());
                    }
                    if (changed) settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                }
            }
        } else if (settings_open && logs_open) {
            if (pressed & SCE_CTRL_CIRCLE) { logs_open = false; restore_settings_parent(); }
            else {
                repeat_scroll(ScrollContext::Logs, std::min((int)diag_log::kCount, 400 / 36));
                if (pressed & SCE_CTRL_UP) settings_row = (settings_row + diag_log::kCount - 1) % diag_log::kCount;
                if (pressed & SCE_CTRL_DOWN) settings_row = (settings_row + 1) % diag_log::kCount;
                if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) {
                    settings.log_mask ^= 1u << settings_row;
                    diag_log::set_mask(settings.log_mask);
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                }
            }
        } else if (settings_open && proxy_open) {
            if (pressed & SCE_CTRL_CIRCLE) {
                tg.cancelProxyCheck(); proxy_open = false; restore_settings_parent(); proxy_imported = false;
                if (proxy_return_to_screen) { settings_open = false; proxy_return_to_screen = false; }
            }
            else {
                const std::vector<int> rows = proxy_rows(proxy_draft.proxy_type);
                const int count = (int)rows.size();
                if (settings_row >= count) settings_row = count - 1;
                const int row = rows[settings_row];
                repeat_scroll(ScrollContext::Proxy, count);
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
                        proxy_scan_row = settings_row;
                        tg.cancelProxyCheck();
                        if (!qr_scanner.start()) settings_error = qr_scanner.status();
                    } else if (proxy_draft.proxy_enabled && !proxy_draft.proxyConfig().complete()) {
                        settings_error = i18n::text("proxy.need_fields");
                    } else if (!proxy_draft.save()) settings_error = i18n::text("settings.save_failed");
                    else {
                        settings = proxy_draft;
                        proxy_imported = false;
                        tg.configureProxy(settings.proxy_enabled, settings.proxyConfig());
                        // The user has now decided how to connect: start the network that was held back.
                        if (network_held && net_ok) { network_held = false; tg.start(true); }
                    }
                }
            }
        } else if (settings_open) {
            if (pressed & SCE_CTRL_CIRCLE) {
                if (confirm_logout || confirm_vault_off) confirm_logout = confirm_vault_off = false;
                else if (settings_section) {
                    settings_section = 0; settings_row = settings_root_row;
                    settings_scroll = settings_root_scroll; settings_error.clear();
                } else settings_open = false;
                pressed = 0;
            }
            const int count = settings_menu::counts[settings_section];
            repeat_scroll(ScrollContext::Settings, std::min(count, kSettingsViewport / kSettingsRowHeight));
            if (pressed & SCE_CTRL_UP) { settings_row = (settings_row+count-1)%count; confirm_logout = false; confirm_vault_off = false; }
            if (pressed & SCE_CTRL_DOWN) { settings_row = (settings_row+1)%count; confirm_logout = false; confirm_vault_off = false; }
            settings_scroll.reveal(settings_row);
            if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) {
                int direction = (pressed & SCE_CTRL_LEFT) ? -1 : 1;
                const int source_row = settings_menu::rows[settings_section][settings_row];
                const int row = source_row - 1;
                if (source_row < 0) {
                    if (pressed & SCE_CTRL_CROSS) {
                        settings_root_row = settings_row; settings_root_scroll = settings_scroll;
                        settings_section = -source_row; settings_row = 0;
                        settings_scroll.reset(); settings_error.clear();
                        confirm_logout = confirm_vault_off = false;
                    }
                } else if (settings_menu::vaultDependent(source_row) && (!vault::enabled() || vault::disabling())) {
                    settings_error = i18n::text("vault.settings.not_enabled");
                } else if (source_row == 32) {
                    if ((pressed & SCE_CTRL_CROSS) && !tg.cacheMaintenance()) {
                        if (!tg.ready()) settings_error = i18n::text("cache.unavailable");
                        else {
                            tg.optimizeCache(AppSettings::days(settings.retention), AppSettings::megabytes(settings.capacity), true);
                            cache_cleanup_requested = true; settings_error.clear();
                            next_cache_cleanup = now + 300000000;
                        }
                    }
                } else if (source_row == 0) {
                    confirm_logout = false;
                    font_picker = false; language_picker = true;
                    theme_picker.count = (int)i18n::languages().size() + 1;
                    int selected = 0;
                    for (int i = 1; i < theme_picker.count; ++i)
                        if (language_choice(i) == settings.language) selected = i;
                    theme_picker.open(selected);
                    settings_error.clear();
                } else if (source_row == 33) {
                    confirm_logout = confirm_vault_off = false;
                    settings.user_colors = !settings.user_colors;
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 2) {
                    confirm_logout = confirm_vault_off = false;
                    settings_error.clear();
                    font_picker = false; language_picker = false;
                    theme_picker.count = ui_themes::count();
                    theme_picker.open(settings.theme);
                } else if (row < 4) {
                    confirm_logout = false;
                    int* values[] = {&settings.retention, &settings.capacity, &settings.theme, &settings.font};
                    const int counts[] = {5, 6, AppSettings::themeChoices(), 7};
                    *values[row] = (*values[row] + counts[row] + direction) % counts[row];
                    apply_font_scale(settings.fontScale());
                    settings_label_marquee.key = settings_value_marquee.key = -1;
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                    next_cache_cleanup = now + 300000000;
                } else if (row == 4) {
                    settings.show_threads = !settings.show_threads;
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 5) {
                    int index = (AppSettings::requestBatchIndex(settings.request_batch_size) + direction + 5) % 5;
                    settings.request_batch_size = AppSettings::requestBatchSize(index);
                    tg.setRequestBatchSize(settings.request_batch_size);
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 6) {
                    if (pressed & SCE_CTRL_CROSS) folder_picker.open(settings.download_dir);
                } else if (row == 7) {
                    if (pressed & SCE_CTRL_CROSS) { save_settings_parent(); proxy_open = true; proxy_return_to_screen = proxy_imported = false; proxy_draft = settings; settings_row = 0; settings_error.clear(); confirm_logout = false; }
                } else if (row == 8) {
                    confirm_logout = false;
                    settings.check_updates = !settings.check_updates;
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 9) {
                    confirm_logout = false;
                    if (pressed & SCE_CTRL_CROSS) {
                        const Updater::Snapshot u = updater.snapshot();
                        settings_error.clear();
                        if (u.state == Updater::State::Available) updater.install();
                        else if (u.busy() && u.state != Updater::State::Installing) updater.cancel();
                        else if (network_held) settings_error = i18n::text("network.held");
                        else if (u.state == Updater::State::Idle || u.state == Updater::State::UpToDate || u.state == Updater::State::Failed) {
                            updater.check();
                            if (settings.proxy_enabled) settings_error = i18n::text("update.direct");
                        }
                    }
                } else if (row == 10) {
                    // Data encryption: switching on/off restarts the app, because existing files are
                    // converted before TDLib starts (see vault_ui::run_migration).
                    confirm_logout = false;
                    if (pressed & SCE_CTRL_CROSS) {
                        const vault_ui::Env vault_env = settings_vault_env();
                        auto finish_vault_flow = [&](vault_ui::Outcome outcome, bool restart) {
                            // Buttons used in the dialog must not leak into the main screen.
                            SceCtrlData after{}; sceCtrlPeekBufferPositive(0, &after, 1); old_buttons = after.buttons; pressed = 0;
                            scroll_input.sample(after.buttons & SCE_CTRL_UP, after.buttons & SCE_CTRL_DOWN, after.ly);
                            scroll_repeat = ScrollRepeat{};
                            lock_previous_frame = lock_last_activity = wall_us(); // a long modal flow is not "left the app"
                            vita2d_set_clear_color((settings.isDark()) ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255));
                            if (outcome == vault_ui::Outcome::Failed) settings_error = i18n::text("vault.error");
                            else if (outcome == vault_ui::Outcome::Done) {
                                settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                                if (restart) { restart_requested = true; tg.close(); settings_open = false; running = false; }
                            }
                        };
                        if (vault::disabling()) settings_error = i18n::text("vault.settings.pending");
                        else if (!vault::enabled()) { confirm_vault_off = false; finish_vault_flow(vault_ui::enable_flow(vault_env), true); }
                        else if (!confirm_vault_off) confirm_vault_off = true;
                        else { confirm_vault_off = false; finish_vault_flow(vault_ui::disable_flow(vault_env), true); }
                    }
                } else if (row == 11) {
                    confirm_logout = false; confirm_vault_off = false;
                    if (pressed & SCE_CTRL_CROSS) {
                        if (!vault::enabled() || vault::disabling()) settings_error = i18n::text("vault.settings.not_enabled");
                        else {
                            const vault_ui::Env vault_env = settings_vault_env();
                            auto outcome = vault_ui::change_flow(vault_env);
                            SceCtrlData after{}; sceCtrlPeekBufferPositive(0, &after, 1); old_buttons = after.buttons; pressed = 0;
                            scroll_input.sample(after.buttons & SCE_CTRL_UP, after.buttons & SCE_CTRL_DOWN, after.ly);
                            scroll_repeat = ScrollRepeat{};
                            lock_previous_frame = lock_last_activity = wall_us(); // a long modal flow is not "left the app"
                            vita2d_set_clear_color((settings.isDark()) ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255));
                            settings_error = outcome == vault_ui::Outcome::Done ? i18n::text("vault.changed") :
                                outcome == vault_ui::Outcome::Failed ? i18n::text("vault.error") : "";
                        }
                    }
                } else if (row == 12) {
                    confirm_logout = false; confirm_vault_off = false;
                    if (pressed & SCE_CTRL_CROSS) {
                        if (!vault::enabled() || vault::disabling()) settings_error = i18n::text("vault.settings.not_enabled");
                        else {
                            vault_ui::check_flow(settings_vault_env());
                            SceCtrlData after{}; sceCtrlPeekBufferPositive(0, &after, 1); old_buttons = after.buttons; pressed = 0;
                            scroll_input.sample(after.buttons & SCE_CTRL_UP, after.buttons & SCE_CTRL_DOWN, after.ly);
                            scroll_repeat = ScrollRepeat{};
                            lock_previous_frame = lock_last_activity = wall_us(); // a long modal flow is not "left the app"
                            vita2d_set_clear_color(settings.isDark() ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255));
                            settings_error.clear();
                        }
                    }
                } else if (row == 13) {
                    // Auto-lock interval; only meaningful with the vault on, but kept either way.
                    confirm_logout = false; confirm_vault_off = false;
                    settings.auto_lock = (settings.auto_lock + AppSettings::kLockChoices + direction) % AppSettings::kLockChoices;
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                    if (!vault::enabled()) settings_error = i18n::text("vault.settings.not_enabled");
                } else if (row == 14) {
                    confirm_logout = false; confirm_vault_off = false;
                    settings.lock_on_leave = !settings.lock_on_leave;
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                    if (!vault::enabled()) settings_error = i18n::text("vault.settings.not_enabled");
                } else if (row == 15) {
                    confirm_logout = false;
                    settings.gyroscope_enabled = !settings.gyroscope_enabled;
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 16) {
                    confirm_logout = false;
                    const int next = settings.opacity + direction * AppSettings::kOpacityStep;
                    settings.opacity = next > AppSettings::kMaxOpacity ? AppSettings::kMinOpacity :
                        next < AppSettings::kMinOpacity ? AppSettings::kMaxOpacity : next;
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 17) {
                    confirm_logout = false;
                    settings.avatar_shape = 1 - settings.avatar_shape;
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 18) {
                    confirm_logout = false;
                    settings.show_archive = !settings.show_archive;
                    tg.setShowArchive(settings.show_archive);
                    if (!tg.currentChatId()) { chat_index = 0; chat_scroll.reset(); }
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 19) {
                    confirm_logout = false;
                    if (pressed & SCE_CTRL_CROSS) { save_settings_parent(); logs_open = true; settings_row = 0; settings_error.clear(); }
                } else if (row == 20) {
                    confirm_logout = false;
                    if (pressed & SCE_CTRL_CROSS) { save_settings_parent(); about_open = true; about_offset = 0; about_link = -1; about_chat_pending = false; settings_error.clear(); }
                } else if (row == 21) {
                    confirm_logout = false;
                    settings.recording_shortcuts = !settings.recording_shortcuts;
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 22) {
                    confirm_logout = false;
                    settings.spoiler_reset = (settings.spoiler_reset + AppSettings::kSpoilerResetChoices + direction) % AppSettings::kSpoilerResetChoices;
                    if (settings.spoiler_reset) invalidate_spoiler_view(spoiler_visibility.reset());
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 23) {
                    confirm_logout = false;
                    settings.scroll_speed = (settings.scroll_speed + ScrollRepeat::kSpeedChoices + direction) % ScrollRepeat::kSpeedChoices;
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 24) {
                    confirm_logout = false;
                    if (pressed & SCE_CTRL_CROSS) {
                        save_settings_parent(); keyboard_settings = 1; settings_row = 0; settings_scroll.reset(); settings_error.clear();
                    }
                } else if (row == 25) {
                    confirm_logout = false;
                    settings.allow_vpk_install = !settings.allow_vpk_install;
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 26) {
                    confirm_logout = false;
                    settings.chat_open_position = (settings.chat_open_position + 3 + direction) % 3;
                    tg.setChatOpenPosition(settings.chat_open_position);
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 27) {
                    confirm_logout = false;
                    settings.combined_view = !settings.combined_view;
                    combined_list_focus = !tg.currentChatId();
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 28) {
                    confirm_logout = false;
                    settings.video_quality = (settings.video_quality + 4 + direction) % 4;
                    tg.setVideoQuality(settings.video_quality);
                    tg.refreshHistory();
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 29) {
                    confirm_logout = false; settings_error.clear();
                    font_files = user_fonts::scan();
                    font_picker = true; font_pending = false; language_picker = false;
                    rich_text_prepare_custom_font("");
                    theme_picker.count = (int)font_files.size() + 1;
                    const auto found = std::find(font_files.begin(), font_files.end(), settings.font_file);
                    theme_picker.open(found == font_files.end() ? 0 : 1 + (int)(found - font_files.begin()));
                } else if (pressed & SCE_CTRL_CROSS) {
                    if (!tg.ready()) settings_error = i18n::text("settings.logout_wait");
                    else if (confirm_logout) { tg.logout(); confirm_logout = false; settings_open = false; }
                    else confirm_logout = true;
                }
            }
        } else if (opening_message && !(history_view.combined && combined_list_focus)) {
            // The loading screen owns input until the requested message is ready.
        } else if (tg.currentChatId() != 0 && !(history_view.combined && combined_list_focus)) {
            const auto& messages = tg.messages();
            if (post_open && !photo_open && !video_fullscreen && !(pressed & SCE_CTRL_CIRCLE) &&
                !messages.empty() && message_index >= 0 && message_index < (int)messages.size()) {
                int next = message_index;
                if (pressed & SCE_CTRL_LTRIGGER) next = std::max(0, message_index-1);
                else if (pressed & SCE_CTRL_RTRIGGER) next = std::min((int)messages.size()-1, message_index+1);
                if ((pressed & SCE_CTRL_LTRIGGER) && message_index <= (tg.historyWindow() ? 0 : 2)) tg.loadOlder(true);
                if ((pressed & SCE_CTRL_RTRIGGER) && message_index + 1 >= (int)messages.size()) tg.loadNewer(true);
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
                    pressed &= ~(SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE | SCE_CTRL_UP | SCE_CTRL_DOWN);
                }
                if ((pressed & SCE_CTRL_LTRIGGER) && message_index <= (tg.historyWindow() ? 0 : 2)) tg.loadOlder(true);
            }
            if (video_fullscreen) {
                if (pressed & (SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE)) video_fullscreen = false;
                if ((pressed & SCE_CTRL_CROSS) && post_open && message_index >= 0 &&
                    message_index < (int)messages.size() && messages[message_index].video) toggle_video = true;
                if (message_index >= 0 && message_index < (int)messages.size()) {
                    if (pressed & SCE_CTRL_LTRIGGER) seek_by(messages[message_index], -10000000);
                    if (pressed & SCE_CTRL_RTRIGGER) seek_by(messages[message_index], 10000000);
                }
            } else if (photo_open) {
                const bool controls = !navigation_handled && !kb.active() && !seek_open && !seek_touch_release &&
                    !confirm_leave && !unsupported_video_prompt && !spoiler_prompt;
                photo_view.sticks(controls ? pad.lx : 128, controls ? pad.ly : 128, controls ? pad.ry : 128, now);
                if (pressed & SCE_CTRL_CIRCLE) photo_open = false;
                if (pressed & SCE_CTRL_TRIANGLE) photo_view.reset();
                if (pressed & SCE_CTRL_LTRIGGER) photo_view.rotate(1);
                else if (pressed & SCE_CTRL_RTRIGGER) photo_view.rotate(-1);
                if (!messages.empty() && message_index >= 0) {
                    const int previous_photo = photo_index, previous_message = message_index;
                    // Arrows walk through every photo of the chat: the rest of this message first, then the
                    // neighbouring messages that have photos (older history is requested at the start).
                    auto step_photo = [&](int direction) {
                        const int next = photo_index + direction;
                        if (next >= 0 && next < (int)messages[message_index].photos.size()) { photo_index = next; return; }
                        for (int j = message_index + direction; j >= 0 && j < (int)messages.size(); j += direction)
                            if (!messages[j].photos.empty()) {
                                message_index = j; post_text_scroll = 0;
                                photo_index = direction > 0 ? 0 : (int)messages[j].photos.size() - 1;
                                return;
                            }
                        if (direction < 0) tg.loadOlder();
                    };
                    if (pressed & SCE_CTRL_LEFT) step_photo(-1);
                    if (pressed & SCE_CTRL_RIGHT) step_photo(1);
                    if (photo_index != previous_photo || message_index != previous_message) photo_view.reset();
                    if (spoiler_locked(messages[message_index])) {
                        photo_open = false; spoiler_prompt = true;
                        spoiler_prompt_chat = tg.currentChatId(); spoiler_prompt_message = messages[message_index].id;
                    } else if (!messages[message_index].photos.empty()) tg.ensureFullPhoto((size_t)message_index, (size_t)photo_index);
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
                repeat_scroll(ScrollContext::Poll, 1); // Poll option heights vary with translated text.
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
                if ((pressed & SCE_CTRL_SELECT) && !(pad.buttons & SCE_CTRL_START)) open_comments();
                if ((pressed & SCE_CTRL_SQUARE) && message_index >= 0 && message_index < (int)messages.size()) {
                    reactions_open = true; reaction_index = 0;
                    tg.loadReactions(messages[message_index].id, true); pressed = 0;
                }
                if (!messages.empty() && message_index >= 0) {
                    int count = (int)messages[message_index].photos.size();
                    if ((pressed & SCE_CTRL_LEFT) && count) photo_index = (photo_index + count - 1) % count;
                    if ((pressed & SCE_CTRL_RIGHT) && count) photo_index = (photo_index + 1) % count;
                    if (!count) {   // Left/Right rewind and fast-forward 10 s in voice and video posts
                        if (pressed & SCE_CTRL_LEFT) seek_by(messages[message_index], -10000000);
                        if (pressed & SCE_CTRL_RIGHT) seek_by(messages[message_index], 10000000);
                    }
                    if (spoiler_locked(messages[message_index]) && messages[message_index].animation &&
                        (pressed & (SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE))) {
                        spoiler_prompt = true; spoiler_prompt_chat = tg.currentChatId();
                        spoiler_prompt_message = messages[message_index].id;
                        pressed &= ~(SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE); toggle_video = false;
                    }
                    if ((pressed & SCE_CTRL_CROSS) && !spoiler_visibility.revealed(tg.currentChatId(), messages[message_index].id) &&
                        std::any_of(messages[message_index].styles.begin(), messages[message_index].styles.end(), [](const StyleSpan& span) { return span.style & TextSpoiler; })) {
                        spoiler_prompt = true; spoiler_prompt_chat = tg.currentChatId(); spoiler_prompt_message = messages[message_index].id;
                        pressed &= ~SCE_CTRL_CROSS;
                    }
                    auto lines = post_lines(font, messages[message_index]);
                    if ((pressed & SCE_CTRL_CROSS) && messages[message_index].voice) toggle_voice = true;
                    if ((pressed & SCE_CTRL_CROSS) && messages[message_index].video) toggle_video = true;
                    if ((pressed & SCE_CTRL_CROSS) && messages[message_index].document)
                        tg.downloadDocument(messages[message_index], settings.download_dir);
                    repeat_scroll(ScrollContext::Post, (544.0-history_view.top) / post_pitch());
                    if (pressed & SCE_CTRL_UP) post_text_scroll -= post_pitch();
                    if (pressed & SCE_CTRL_DOWN) post_text_scroll += post_pitch();
                    post_text_scroll = std::max(0, std::min(post_text_scroll, post_scroll_max(messages[message_index], lines.size())));
                    int link_line = std::max(0, (post_text_scroll-post_text_top(messages[message_index])+62)/post_pitch());
                    const MessageItem& current = messages[message_index];
                    if ((pressed & SCE_CTRL_TRIANGLE) && ((current.voice && !current.audio) || current.video_note) &&
                        current.transcription.empty() && !current.transcription_pending && !current.transcription_complete) {
                        tg.requestTranscription(current.id);   // the text arrives inside the post's text panel
                    } else if (pressed & SCE_CTRL_TRIANGLE) {
                        if (messages[message_index].video && !messages[message_index].video_note) video_fullscreen = true;
                        else if (link_line < (int)lines.size() && !lines[link_line].url.empty())
                            browser_error = open_link(lines[link_line].url);
                        else if (!current.preview_url.empty()) browser_error = open_link(current.preview_url);
                    }
                    if ((pressed & SCE_CTRL_CROSS) && count) {
                        if (spoiler_locked(messages[message_index])) {
                            spoiler_prompt = true;
                            spoiler_prompt_chat = tg.currentChatId(); spoiler_prompt_message = messages[message_index].id;
                        } else {
                            tg.ensureFullPhoto((size_t)message_index, (size_t)photo_index);
                            photo_open = true;
                            photo_view.begin();
                        }
                    }
                }
            } else {
                if (pressed & SCE_CTRL_CIRCLE) { if (tg.inComments()) tg.closeComments(); else if (tg.inTopic()) tg.backToTopics(); else if (history_view.combined) combined_list_focus = true; else tg.leaveChat(); pressed = 0; }
                if (pressed & SCE_CTRL_SQUARE) tg.requestCompose();
                if ((pressed & SCE_CTRL_SELECT) && !(pad.buttons & SCE_CTRL_START) && tg.ready()) { upload_picker.openMenu(); chat_menu_open = false; }
                if (settings.recording_shortcuts && (pressed & SCE_CTRL_LTRIGGER)) open_circle();
                else if (settings.recording_shortcuts && (pressed & SCE_CTRL_RTRIGGER)) record_toggle = true;
                if (pressed & SCE_CTRL_TRIANGLE) tg.refreshHistory();
                double history_page_steps = 1;
                if (scroll_direction.direction && !messages.empty()) {
                    const int current = std::clamp(message_index, 0, (int)messages.size() - 1);
                    const int next = std::clamp(current + scroll_direction.direction, 0, (int)messages.size() - 1);
                    const int height = std::max(history_message_height(font, messages, current, tg.currentChatIsGroup()),
                                                history_message_height(font, messages, next, tg.currentChatIsGroup())) + 26;
                    history_page_steps = (history_bottom-history_view.top) / (double)height;
                }
                repeat_scroll(ScrollContext::History, history_page_steps);
                if (pressed & SCE_CTRL_UP) {
                    if (message_index > 0) message_index--;
                    if (message_index <= (tg.historyWindow() ? 0 : 2)) tg.loadOlder();
                }
                if (pressed & SCE_CTRL_DOWN) {
                    if (message_index + 1 < (int)messages.size()) message_index++;
                    if (message_index + 1 >= (int)messages.size()) tg.loadNewer();
                }
                if (pressed & SCE_CTRL_RIGHT) open_comments();
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
        } else if (search_open && tg.canBrowse()) {
            const int count = (int)tg.searchResults().size();
            repeat_scroll(ScrollContext::Search, kSearchRows);
            if (pressed & SCE_CTRL_UP) search_index--;
            if (pressed & SCE_CTRL_DOWN) search_index++;
            search_index = std::max(0, std::min(search_index, count - 1));
            if ((pressed & SCE_CTRL_CROSS) && count) {
                const ChatItem chat = tg.searchResults()[search_index];
                tg.openChat(chat.id, chat.title);
                topic_index = 0; message_index = -1; post_open = photo_open = false;
                search_open = false;
                combined_list_focus = false;
            } else if (pressed & SCE_CTRL_SQUARE) {
                search_input = true; kb.open(tr("search.kb_title"), false, tg.searchQuery());
            } else if (pressed & SCE_CTRL_TRIANGLE) tg.searchChats(tg.searchQuery());
            else if (pressed & SCE_CTRL_CIRCLE) { search_open = false; tg.clearSearch(); }
        } else if (tg.canBrowse()) {
            // The search field is the row above the first chat: Up reaches it, Down leaves it, Cross opens it.
            repeat_scroll(ScrollContext::Chats, tg.folders().empty() ? 10 : 9);
            if (search_bar_focus) {
                if (pressed & SCE_CTRL_DOWN) search_bar_focus = false;
                if (pressed & SCE_CTRL_CROSS) pressed |= SCE_CTRL_SQUARE;
                pressed &= ~(SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_CROSS);
            } else if ((pressed & SCE_CTRL_UP) && chat_index <= 0) {
                search_bar_focus = true; pressed &= ~SCE_CTRL_UP;
            }
            if (pressed & SCE_CTRL_SQUARE) {
                if (tg.currentChatId()) tg.leaveChat();
                search_bar_focus = false;
                search_open = true; search_input = true; search_index = 0;
                tg.clearSearch(); kb.open(tr("search.kb_title"));
            }
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
                    if (tg.currentChatId() != chats[chat_index].id || tg.inComments() || tg.inTopic()) {
                        tg.openChat(chats[chat_index].id, chats[chat_index].title);
                        topic_index = 0; message_index = -1;
                        post_open = photo_open = false;
                    }
                    post_open = photo_open = false;
                    combined_list_focus = false;
                }
            }
            if (history_view.combined && (pressed & SCE_CTRL_CIRCLE) && tg.currentChatId()) tg.leaveChat();
            if (pressed & SCE_CTRL_TRIANGLE) tg.refreshChats();
        }

        if (record_toggle && !circle_recorder.active() && !recorder.active() && tg.currentChatId() && !kb.active() && !photo_open)
            open_audio();
        if (recorder.active() && (!tg.ready() || tg.currentChatId() != audio_chat || tg.currentTopicId() != audio_topic))
            recorder.cancel();
        recorder.pump(now);
        observe_spoiler_chat(); // Input may have left/switched the chat after pump().

        std::vector<UploadPicker::Item> upload_batch;
        std::string upload_caption;
        if (upload_picker.takeResult(upload_batch, &upload_caption)) {
            // Photos and videos go compressed in one album message; other files stay untouched documents.
            std::vector<std::pair<std::string, int>> files;
            for (const auto& item : upload_batch) files.emplace_back(item.path, (int)item.kind);
            if (!tg.sendLocalFiles(files, tg.currentChatId(), tg.currentTopicId(), upload_caption))
                browser_error = i18n::text("upload.send_failed");
        }

        if (radial_flow && !upload_picker.active() && !stickers_open && !gifs_open && !recorder.active() &&
            !kb.active() && tg.inputKind() != InputKind::Message) {
            tg.cancelInput(); radial_flow = false;
        }

        if (exit_gesture.update(pad.buttons & SCE_CTRL_START, pad.buttons & SCE_CTRL_SELECT,
                pressed & SCE_CTRL_SELECT, kb.active(), now)) {
            tg.close();
            running = false;
        }

        // The modal blocks underlying actions this frame, but must allow the next touch release.
        if (restore_webapp_touch && tg.botWebApp().phase != TelegramClient::BotWebApp::Closed) touch_blocked = false;
        input_actions.finish();
        // Refresh only when TDLib rotates the login token, between GPU frames.
        if (auth_qr_text != tg.qrLink()) {
            auth_qr_text = tg.qrLink();
            vita2d_wait_rendering_done();
            if (auth_qr_texture) { vita2d_free_texture(auth_qr_texture); auth_qr_texture = nullptr; }
            auth_qr = QrCode();
            if (!auth_qr_text.empty()) {
                const bool high = qr_encode(auth_qr_text, auth_qr, QrEcc::High);
                if (high) {
                    const auto image = qr_render_login(auth_qr, std::max(2, 400/(auth_qr.size+8)));
                    auth_qr_texture = vita2d_create_empty_texture(image.side, image.side);
                    if (auth_qr_texture) {
                        auto* pixels = static_cast<uint32_t*>(vita2d_texture_get_datap(auth_qr_texture));
                        const unsigned stride = vita2d_texture_get_stride(auth_qr_texture)/4;
                        for (int y=0; y<image.side; ++y) for (int x=0; x<image.side; ++x) {
                            const uint32_t v = image.pixels[size_t(y)*image.side+x];
                            pixels[size_t(y)*stride+x] = 0xff000000u | v | (v<<8) | (v<<16);
                        }
                        vita2d_texture_set_filters(auth_qr_texture, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
                    }
                } else {
                    // Keep a plain QR available for an unexpectedly long token.
                    qr_encode(auth_qr_text, auth_qr);
                }
            }
        }
        ui_stage.to(ui_diagnostics::Phase::Media);
        performance::SlowScope media_update(performance::SlowPhase::Ui,"media_update");
        if (qr_scanner.active()) {
            std::string scanned = qr_scanner.takeLink();
            if (!scanned.empty()) { pending_proxy_link = scanned; qr_scanner.stop(); }
            else qr_scanner.upload();
        }
        if (!pending_proxy_link.empty()) {
            ProxyLink proxy; i18n::Text error;
            auto parsed = parse_proxy_link(pending_proxy_link, proxy, error);
            pending_proxy_link.clear();
            if (parsed == ProxyLinkResult::Valid) {
                proxy_return_to_screen = proxy_return_to_screen || !settings_open;
                if (settings_open && !proxy_open) save_settings_parent();
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
            browser_error.clear(); browser_notice.clear();
            tg.dismissStatusNotices();
            if (!recorder.active()) recorder.setStatus("");
        }
        const std::string browser_status = browser_notice.update(browser_error, now);

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

        if (!settings_open && !tg.historyAnchorPending() && post_open && !photo_open && tg.currentChatId() && message_index>=0 && message_index<(int)tg.messages().size())
            tg.viewMessage(tg.messages()[message_index].id);
        const bool sticker_preview = !settings_open && stickers_open && !sticker_pack_menu &&
            sticker_preview_index >= 0 && sticker_preview_index < (int)tg.stickers().size();
        const SendStickerItem* preview_sticker = sticker_preview ? &tg.stickers()[sticker_preview_index] : nullptr;
        if (preview_sticker) tg.ensureSticker(sticker_preview_index, true);
        const bool gif_preview = !settings_open && gifs_open && gif_preview_index >= 0 &&
            gif_preview_index < (int)tg.gifs().items.size();
        const SendGifItem* preview_gif = gif_preview ? &tg.gifs().items[gif_preview_index] : nullptr;
        if (preview_gif) tg.ensureGif(gif_preview_index, true);
        int animation_index = !stickers_open && !gifs_open && !circle_recorder.active() && !recorder.active() && !settings_open && post_open && !photo_open && !kb.active() && running &&
            message_index >= 0 && message_index < (int)tg.messages().size() && tg.messages()[message_index].animation && !spoiler_locked(tg.messages()[message_index]) ? message_index : -1;
        tg.setAnimationTarget(animation_index, animation_index >= 0);
        const MessageItem* animation_message = animation_index >= 0 ? &tg.messages()[animation_index] : nullptr;
        const bool metadata_unsupported_video = animation_message && animation_message->video && !animation_message->video_note &&
            animation_message->video_width > 0 && animation_message->video_height > 0 &&
            !VideoQuality::hardwareRasterSupported(animation_message->video_width, animation_message->video_height);
        if (animation_message && animation_message->video && !animation_message->video_note && !metadata_unsupported_video &&
            animation.preparationRequired() && (tg.currentChatId() != raster_mismatch_chat || animation_message->id != raster_mismatch_message)) {
            raster_mismatch_chat = tg.currentChatId(); raster_mismatch_message = animation_message->id;
        }
        const bool unsupported_video = metadata_unsupported_video || (animation_message && animation_message->video &&
            !animation_message->video_note && tg.currentChatId() == raster_mismatch_chat && animation_message->id == raster_mismatch_message);
        const bool prepare_video = unsupported_video && portrait_preparation_requested &&
            tg.currentChatId() == portrait_preparation_chat && animation_message->id == portrait_preparation_message;
        if (portrait_preparation_requested && !prepare_video) portrait_preparation_requested = false;
        if (unsupported_video_prompt && (!animation_message || tg.currentChatId() != unsupported_video_chat ||
            animation_message->id != unsupported_video_message)) unsupported_video_prompt = false;
        const auto* picker_focus = upload_picker.stage == UploadPicker::Stage::Review ? upload_picker.focusedItem() : nullptr;
        const bool picker_video = picker_focus && picker_focus->kind == UploadPicker::Kind::Video;
        AnimationPlayer& media_animation = !picker_video && animation_message && animation_message->video_note && (toggle_video ||
            !bg_media.live || !bg_media.circle || bg_media.file == animation_message->animation_file_id) ? video_note_animation : animation;
        auto animation_stream = animation_message && !unsupported_video && animation_message->animation_error.empty() &&
            animation_message->animation_stream && animation_message->animation_stream->ready() ? animation_message->animation_stream : nullptr;
        // Navigation and silent previews must not replace a playing video note.
        if (&media_animation == &video_note_animation) animation.setSource("");
        // Non-video mode starts immediately and loops silently for sticker and GIF previews.
        if (preview_sticker) animation.setSource(preview_sticker->animated ? preview_sticker->full_path : "");
        else if (preview_gif) animation.setSource(preview_gif->full_path);
        else if (picker_video) animation.setSource(picker_focus->path, true, false, false);
        else media_animation.setSource(animation_stream ? animation_stream->source : animation_message && animation_message->animation_error.empty() ? animation_message->animation_path : "",
            animation_message && animation_message->video, animation_message && animation_message->video_note, unsupported_video, unsupported_video && !prepare_video, animation_stream,
            animation_message && animation_message->video && animation_message->animation_error.empty() ? animation_message->animation_file_id : 0);
        if (portrait_play_pending && (!animation_message || !unsupported_video ||
            tg.currentChatId() != portrait_play_chat || animation_message->id != portrait_play_message))
            portrait_play_pending = false;
        if (unsupported_video && !prepare_video && toggle_video && !media_animation.preparationChecked()) {
            portrait_play_pending = true;
            portrait_play_chat = tg.currentChatId(); portrait_play_message = animation_message->id;
            toggle_video = false;
        }
        if (portrait_play_pending && media_animation.preparationChecked()) {
            portrait_play_pending = false;
            toggle_video = true;
        }
        if (unsupported_video && !prepare_video && media_animation.preparationRequired() && (toggle_video || video_fullscreen)) {
            video_fullscreen = false;
            toggle_video = false;
            unsupported_video_prompt = true;
            unsupported_video_chat = tg.currentChatId();
            unsupported_video_message = animation_message->id;
        }

        if (toggle_video) quality_resume_message = 0;
        if (quality_resume_message) {
            if (!animation_message || tg.currentChatId() != quality_resume_chat || animation_message->id != quality_resume_message ||
                animation_message->animation_file_id != quality_resume_file || media_animation.failedToOpen()) quality_resume_message = 0;
            else if (media_animation.readyForPlayback() && (!quality_resume_time ||
                     (media_animation.canSeek() && media_animation.seekLimitUs() >= quality_resume_time))) {
                if (quality_resume_time) media_animation.seekTo(quality_resume_time);
                toggle_video = quality_resume_playing;
                quality_resume_message = 0;
            }
        }
        if (toggle_video && (animation_message || picker_video)) {
            media_animation.toggle();
            if (media_animation.playing()) {
                auto& other = &media_animation == &animation ? video_note_animation : animation;
                if (other.playing()) other.toggle();
            }
        }
        animation.upload();
        video_note_animation.upload();
        if (&media_animation == &video_note_animation && (video_note_animation.playing() || bg_media.circle)) {
            const bool same = bg_media.circle && bg_media.file == animation_message->animation_file_id;
            bg_media.live = video_note_animation.playing() || (same && bg_media.live);
            bg_media.circle = true; bg_media.message = animation_message->id; bg_media.duration = animation_message->video_duration;
            bg_media.title = tr("chat.thumb.round"); bg_media.file = animation_message->animation_file_id;
            bg_media.stream.reset();
            if (video_note_animation.playing()) { tg.clearAudioQueue(); bg_autoplay = false; }
        } else if (bg_media.circle) {
            const bool ended = bg_media.duration > 0 && !video_note_animation.playing() &&
                video_note_animation.positionUs() >= (int64_t)bg_media.duration * 1000000 - 500000;
            bg_media.live = bg_media.live && !ended && !video_note_animation.failedToOpen();
        }
        int voice_index = !circle_recorder.active() && !recorder.active() && !settings_open && post_open && !photo_open && !kb.active() && running && tg.currentChatId() &&
            message_index >= 0 && message_index < (int)tg.messages().size() && tg.messages()[message_index].voice ? message_index : -1;
        if (!tg.currentChatId()) history_voice_id = 0;
        if (voice_index < 0 && history_voice_id && !circle_recorder.active() && !recorder.active() && !settings_open && !kb.active() && running && !photo_open) {
            // Started from the chat list: keep that message as the playback source until another one takes over.
            for (size_t k = 0; k < tg.messages().size(); ++k) if (tg.messages()[k].id == history_voice_id) voice_index = (int)k;
            if (voice_index < 0 && !tg.messages().empty()) history_voice_id = 0;
        }
        tg.setVoiceTarget(voice_index);
        const MessageItem* voice_message = voice_index >= 0 ? &tg.messages()[voice_index] : nullptr;
        // Music started elsewhere keeps playing when the selection lands on another voice message (or another chat opens);
        // only pressing play on that message takes over.
        if (voice_message && bg_media.live && (bg_media.circle || voice_message->voice_file_id != bg_media.file) &&
            !(toggle_voice || history_voice_toggle)) voice_message = nullptr;
        if (diag_log::enabled(diag_log::Kind::Performance)) {
            static int32_t traced_file = 0;
            static bool traced_ready = false, traced_complete = false;
            const auto state = voice_message && voice_message->voice_stream ? voice_message->voice_stream->snapshot() : download_stream::State::Snapshot{};
            const int32_t file = voice_message ? voice_message->voice_file_id : 0;
            const bool complete = voice_message && (!voice_message->voice_path.empty() || state.complete);
            const bool ready = complete || (voice_message && voice_message->voice_stream && voice_message->voice_stream->ready());
            if (file != traced_file || ready != traced_ready || complete != traced_complete) {
                audio_startup_trace::record(&voice, 0, complete ? "download_complete" : ready ? "download_ready" : "download_wait",
                    ready, 0, voice_message ? voice_message->voice_stream : nullptr, file);
                traced_file = file; traced_ready = ready; traced_complete = complete;
            }
            if ((toggle_voice || history_voice_toggle) && voice_message)
                audio_startup_trace::record(&voice, 0, "ui_play_button", ready, 0, voice_message->voice_stream, file);
        }
        auto voice_stream = voice_message && voice_message->voice_error.empty() && voice_message->voice_stream &&
            voice_message->voice_stream->ready() ? voice_message->voice_stream : nullptr;
        // Keep the queue authoritative while browsing another message or chat.
        if (voice_message && voice_message->audio && (voice.playing() || toggle_voice || history_voice_toggle)) {
            const auto* current = tg.audioTrack();
            if (!current || current->file != voice_message->voice_file_id || tg.audioTrackIndex(current->message) != voice_index) {
                tg.startAudioQueue(voice_index); bg_autoplay = false;
            }
        } else if (voice_message && !voice_message->audio) { tg.clearAudioQueue(); bg_autoplay = false; }
        const auto* queued = tg.audioTrack();
        if (queued && !bg_autoplay && voice.finished() && !bg_finished_handled && bg_media.file == queued->file) {
            bg_finished_handled = true;
            if (voice.error().empty() && tg.advanceAudioQueue()) {
                queued = tg.audioTrack();
                voice.setSource(""); history_voice_id = 0;
                bg_autoplay = true; voice_message = nullptr;
            }
        }
        tg.serviceAudioQueue();
        const bool keep_voice = !voice_message && bg_media.live && !bg_media.circle;
        std::string queued_source;
        auto queued_stream = queued && queued->stream && queued->stream->ready() ? queued->stream : nullptr;
        const bool use_queue = queued && (keep_voice || (voice_message && voice_message->audio));
        if (use_queue) {
            queued_source = queued_stream ? queued_stream->source : queued->path;
            voice.setSource(queued_source, false, true, queued_stream);
            bg_media.circle = false;
            bg_media.title = queued->title; bg_media.duration = queued->duration; bg_media.file = queued->file; bg_media.message = queued->message;
            bg_media.stream = queued->stream;
        } else if (keep_voice && bg_media.stream && bg_media.stream->ready()) {
            voice.setSource(bg_media.stream->source, false, false, bg_media.stream);
            queued_source = bg_media.stream->source;
        } else if (!keep_voice)
            voice.setSource(voice_stream ? voice_stream->source : voice_message && voice_message->voice_error.empty() ? voice_message->voice_path : "", false,
                voice_message && voice_message->audio, voice_stream);
        if ((toggle_voice || history_voice_toggle) && voice_message) voice.toggle();
        if ((toggle_voice || history_voice_toggle) && voice_message && !voice_stream && voice_message->voice_path.empty() && voice_message->voice_error.empty()) bg_autoplay = true;
        const bool animation_playing = animation.playing() || video_note_animation.playing();
        if (animation_playing && !prev_anim_playing) bg_autoplay = false;
        if (bg_autoplay && !voice.playing() && !voice.finished() && (!queued_source.empty() || (voice_message && (voice_stream || !voice_message->voice_path.empty())))) {
            voice.toggle(); bg_autoplay = false;
        } else if (voice.playing()) bg_autoplay = false;
        if (voice.finished() && !voice.error().empty()) bg_autoplay = false;
        if (!voice.finished()) bg_finished_handled = false;
        // One sound output: whichever of music/voice and sound video (clip or video note) started last pauses the other.
        if (voice.playing() && animation_playing) {
            if (!prev_anim_playing) voice.setPlaying(false);
            else {
                if (animation.playing()) animation.toggle();
                if (video_note_animation.playing()) video_note_animation.toggle();
            }
        }
        prev_anim_playing = animation.playing() || video_note_animation.playing();
        if (voice_message) {
            const bool same = !bg_media.circle && bg_media.message == voice_message->id;
            bg_media.live = voice.playing() || (same && bg_media.live) || bg_autoplay;
            bg_media.circle = false; bg_media.message = voice_message->id; bg_media.duration = voice_message->voice_duration;
            bg_media.title = voice_message->audio ? voice_message->audio_title : i18n::Text(tr("audio.voice_message"));
            bg_media.file = voice_message->voice_file_id;
            bg_media.stream = voice_message->voice_stream;
        } else if (!bg_media.circle) bg_media.live = keep_voice || bg_autoplay;
        update_audio_bar();
        tg.setBackgroundMedia(bg_media.live && !bg_media.circle ? bg_media.file : 0, bg_media.live && bg_media.circle ? bg_media.file : 0);
        if (!bg_media.live && !voice_message) tg.clearAudioQueue();
        history_voice_toggle = false;

        const media_seek::Source fullscreen_source{tg.currentChatId(), animation_message ? animation_message->id : 0};
        const bool fullscreen_seek_enabled = video_fullscreen && animation_message && animation_message->video &&
            animation_message->video_duration > 0 && media_animation.canSeek() && !navigation_handled &&
            !chat_menu_open && !seek_open && !seek_touch_release && !confirm_leave && !unsupported_video_prompt &&
            !spoiler_prompt && pending_web_link.empty() && !kb.active() && !settings_open &&
            !stickers_open && !gifs_open && !reactions_open && !upload_picker.active() && !circle_recorder.active() &&
            !recorder.active() && !touching && !touch_down && !scrub_kind && !toggle_video;
        const bool seek_left = pad.buttons & SCE_CTRL_LEFT, seek_right = pad.buttons & SCE_CTRL_RIGHT;
        const auto seek_event = fullscreen_seek.update(fullscreen_source, fullscreen_seek_enabled,
            seek_left == seek_right ? 0 : seek_left ? -1 : 1,
            physical_pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT), seek_left || seek_right,
            media_animation.positionUs(), animation_message ? std::min<int64_t>(int64_t(animation_message->video_duration) * 1000000,
                media_animation.seekLimitUs() + 500000) : 0, now);   // while streaming, the cursor stops at the downloaded part
        using SeekAction = media_seek::HoldSeek::Action;
        if (seek_event.action == SeekAction::Begin) {
            fullscreen_seek_was_playing = media_animation.playing();
            if (fullscreen_seek_was_playing) media_animation.toggle();
        } else if ((seek_event.action == SeekAction::Commit || seek_event.action == SeekAction::Cancel) &&
                   seek_event.source == fullscreen_source && media_animation.canSeek()) {
            if (seek_event.action == SeekAction::Commit) media_animation.seekTo(seek_event.target);
            // A newly opened time dialog or an explicit Play/Pause action owns the pause state.
            if (!seek_open && !toggle_video && media_animation.playing() != fullscreen_seek_was_playing) media_animation.toggle();
        }
        if (fullscreen_seek.active() || seek_event.action == SeekAction::Commit) {
            media_active_at = now;
            lock_last_activity = wall_us();
        }

        // Playback needs no touches: keep the system's dim/off/suspend timers
        // alive as well as the vault idle timer. Pausing/ending restores normal idle behaviour.
        const bool video_playing = animation.playing() || video_note_animation.playing() || (circle_playback && circle_playback->playing());
        if (!video_playing) next_video_power_tick = 0;
        else if (now >= next_video_power_tick) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
            next_video_power_tick = now + 1000000;
        }

        if (pressed || touching || tg.currentChatId() || settings_open || !tg.ready())
            next_cache_cleanup = now + 300000000;
        if (tg.ready() && !tg.currentChatId() && !settings_open && now >= next_cache_cleanup) {
            tg.optimizeCache(AppSettings::days(settings.retention), AppSettings::megabytes(settings.capacity));
            next_cache_cleanup = now + 300000000;
        }
        if (skin_theme != settings.theme || ui_skin::needs_retry()) {
            if (skin_theme >= 0 && skin_theme != settings.theme) theme_switch_screen(font);
            skin_theme = settings.theme;
            ui_skin::load(settings.theme);
            ui_battery::load(settings.theme);
            rich_text_skin_changed();
        }
        // Defer palette file I/O while video playback competes with TDLib downloads.
        if (!video_playing) ui_skin::reload_palette();
        bool dark = ui_skin::is_dark(settings.isDark());
        ui_skin::set_opacity(settings.opacity);
        const unsigned int white = ui_skin::color("surface", dark ? RGBA8(31,45,59,255) : RGBA8(255,255,255,255));
        const unsigned int ink = ui_skin::color("text", dark ? RGBA8(231,238,245,255) : RGBA8(39,51,66,255));
        const unsigned int muted = ui_skin::color("muted", settings.theme == 2 ? RGBA8(194,207,237,255) : dark ? RGBA8(163,183,200,255) : RGBA8(105,122,140,255));
        const unsigned int accent = ui_skin::color("accent", settings.theme == 2 ? RGBA8(149,214,255,255) : RGBA8(35,135,210,255));
        const unsigned int selected = ui_skin::color("selected", dark ? RGBA8(46,72,94,255) : RGBA8(207,232,249,255));
        const unsigned int outgoing = ui_skin::color("green", RGBA8(20,143,82,255));
        const unsigned int separator = ui_skin::color("divider", dark ? RGBA8(53,70,86,255) : RGBA8(205,216,226,255));
        vita2d_set_clear_color(dark ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255));
        // Textures are swapped here, between frames, only when the theme actually changed.
        // Fullscreen photo and video share one rule: the corner button hides after 1 s without presses or touches.
        const bool hide_media_ui = (photo_open || video_fullscreen) && !chat_menu_open && !fullscreen_seek.active() && now - media_active_at > 1000000;
        scrub_voice.on = scrub_bar.on = scrub_play.on = scrub_ring.on = reply_hit.on = delete_hit.on = false;
        voice_play_rects.clear();   // republished by this frame's controls
        comments_rects.clear();
        reply_rects.clear();
        const bool scrub_active = touching && scrub_kind >= 1 && scrub_kind <= 3;
        struct VideoProgress { int64_t duration_us, position_us; float fraction; bool seekable, scrubbing; };
        auto video_progress = [&](const MessageItem& m) {
            VideoProgress state{(int64_t)m.video_duration * 1000000, media_animation.positionUs(), 0.f, false, false};
            if (m.video) media_animation.setDurationHint(state.duration_us);
            state.seekable = media_animation.canSeek() && state.duration_us > 0;
            if (video_fullscreen && fullscreen_seek.active()) state.position_us = fullscreen_seek.targetUs();
            state.scrubbing = scrub_active && scrub_kind != 1 && state.seekable;
            state.fraction = state.duration_us > 0 ? (float)std::max<int64_t>(0, std::min(state.position_us, state.duration_us)) / state.duration_us : 0.f;
            if (state.scrubbing) { state.fraction = scrub_fraction; state.position_us = (int64_t)(scrub_fraction * state.duration_us); }
            return state;
        };
        {
            const int64_t elapsed = media_ui_stamp ? std::min<int64_t>(now - media_ui_stamp, 100000) : 0;
            media_ui_stamp = now;
            const float step = elapsed / 100000.f;
            media_ui_alpha = hide_media_ui ? std::max(0.f, media_ui_alpha - step) : std::min(1.f, media_ui_alpha + step);
            if (!(photo_open || video_fullscreen)) media_ui_alpha = 1.f;
        }
        float tr_reveal = 1.f;   // 0..1 progress of the transcription reveal in the open post
        if (post_open && message_index >= 0 && message_index < (int)tg.messages().size()) {
            const MessageItem& tracked = tg.messages()[message_index];
            const bool has_text = !tracked.transcription.empty();
            if (tracked.id != tr_track_id) { tr_track_id = tracked.id; tr_track_has = has_text; tr_anim_since = 0; }
            else if (has_text && !tr_track_has) { tr_track_has = true; tr_anim_since = now; }
            else if (!has_text) tr_track_has = false;
            if (tr_anim_since) {
                const float t = std::min(1.f, (float)(now - tr_anim_since) / 180000.f);
                tr_reveal = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
                if (t >= 1.f) tr_anim_since = 0;
            }
        }
        if (textures.avatarShape() != settings.avatar_shape) { vita2d_wait_rendering_done(); textures.setAvatarShape(settings.avatar_shape); }

        media_update.finish();
        update_performance_context(scroll_direction.direction != 0);
        ui_stage.to(ui_diagnostics::Phase::Draw);
        performance::SlowScope draw_stage(performance::SlowPhase::Ui,"draw");
        performance::SlowScope draw_begin(performance::SlowPhase::Ui,"draw_begin");
        vita2d_start_drawing();
        vita2d_clear_screen();
        drawer_draw_shift = combined_visible() && !tg.folders().empty() ? (int)std::lround(folder_drawer.shift(now)) : 0;
        const int drawer_draw_width = combined_visible() && !tg.folders().empty() ? drawer_draw_shift+chat_view::FolderDrawer::rail : 0;
        sceGxmSetViewport(vita2d_get_context(), 480.f, 480.f, 272.f, -272.f, 0.5f, 0.5f);
        draw_begin.finish();
        Keyboard* keyboard_screen = kb.ownsScreen() ? &kb : nullptr;
#if VITAGRAM_WEBKIT
        if (!keyboard_screen && webapp_keyboard.ownsScreen()) keyboard_screen = &webapp_keyboard;
#endif
        auto finish_frame = [&] {
            performance::SlowScope draw_end(performance::SlowPhase::Ui,"draw_end");
            vita2d_end_drawing();
            draw_end.finish();
            draw_stage.finish();
            slow_ui.finish();
            ui_measurement.finish();
            if (input_sampled) performance::observe(performance::Metric::InputSubmit, performance::now()-input_sampled);
            // vita2d reuses one vertex pool on the next frame. Finish this scene
            // before the IME renders into the same surfaces or that pool is reset.
            ui_stage.to(ui_diagnostics::Phase::Swap);
            { performance::Scope measurement(performance::Metric::GpuWait); vita2d_wait_rendering_done(); }
            const bool system_keyboard_active = kb.systemActive()
#if VITAGRAM_WEBKIT
                || webapp_keyboard.systemActive()
#endif
                ;
            if (system_keyboard_active) {
                performance::SlowScope ime_update(performance::SlowPhase::Ui,"ime_update");
                vita2d_common_dialog_update();
            }
            { performance::Scope measurement(performance::Metric::Swap); vita2d_swap_buffers(); }
            if (!bot_panel && tg.botWebApp().phase == TelegramClient::BotWebApp::Closed && !settings_open && !tg.historyAnchorPending() && !stickers_open && !gifs_open && !reactions_open && !tg.topicsOpen() && !circle_recorder.active() && !recorder.active() && !photo_open && !kb.active())
                tg.messagesPresented(message_index >= 0 && message_index < (int)tg.messages().size() ? tg.messages()[message_index].id : 0);
            // CommonDialog submits through its own context; waiting only for the
            // vita2d context doesn't cover the keyboard's GPU work.
            if (system_keyboard_active) { performance::Scope measurement(performance::Metric::GpuWait); sceGxmDisplayQueueFinish(); }
            {
                ui_stage.to(ui_diagnostics::Phase::FrameTail);
                performance::Scope measurement(performance::Metric::FrameTail);
                performance::SlowScope slow(performance::SlowPhase::Ui, "frame_tail");
                if (!keyboard_interaction && !keyboard_screen) textures.loadOne();
                { performance::Scope text(performance::Metric::TextPoll); rich_text_poll(); }
                if (!interaction && !keyboard_interaction && !keyboard_screen && !decode_cooldown) emoji_load_one();
            }
        };
        if (keyboard_screen) {
            vita2d_disable_clipping();
            {
                performance::SlowScope keyboard_draw(performance::SlowPhase::Ui,"keyboard_draw");
                keyboard_screen->draw(font);
            }
            finish_frame();
            continue;
        }
        ui_skin::background();
        performance::SlowScope draw_header(performance::SlowPhase::Ui,"draw_header");

        if (!ui_skin::header(ui_layout::header_height))
            vita2d_draw_rectangle(0, 0, ui_layout::width, ui_layout::header_height, dark ? white : accent);
        int title_x = 20;
        if (tg.currentChatId()) {
            if (!decode_cooldown) for (size_t i=0; i<tg.chats().size(); ++i)
                if (tg.chats()[i].id == tg.currentChatId()) { tg.ensureChatMedia(i); break; }
            auto avatar = textures.get(tg.currentChatAvatar(), kMaskAvatar);
            if (tg.currentChatIsDeleted()) draw_deleted_avatar(12, 4, 28, settings.avatar_shape);
            else if (avatar) draw_texture_fit(avatar, 12, 4, 28, 28);
            else draw_avatar_placeholder(12, 4, 28, settings.user_colors ? peer_colors::rgba(tg.senderColors(tg.currentChatId(), true, dark).rgb[0]) : selected, settings.avatar_shape);
            title_x = 50;
        }
        const unsigned int header_text = ui_skin::color("header_text", RGBA8(240,247,255,255));
        std::string heading = settings_open ? (proxy_open ? tr("proxy.title") : logs_open ? tr("logs.title") : about_open ? tr("about.title") : keyboard_settings ? tr(keyboard_settings == 2 ? "settings.keyboard_languages" : "settings.keyboard") : tr(settings_menu::titles[settings_section])) : tg.currentChatId() ? std::string(tg.currentChatTitle()) : "VitaGram " VITAGRAM_VERSION;
        std::string presence = settings_open ? "" : tg.currentUserStatus();
        if (combined_visible() && !post_open) {
            presence = tr(combined_list_focus ? "chat.focus_list" : "chat.focus_history");
            if (!folder_drawer.open) for (const auto& folder : tg.folders()) {
                if (folder.id == tg.selectedFolder()) { presence = folder.title; break; }
            }
        }
        // Separate title, centered presence/network status and right-aligned device status.
        constexpr float header_scale = 0.65f;
        draw_text(font, title_x, ui_layout::header_baseline, fit_text(font, settings_open ? tr("settings.title") : heading, 320-title_x, header_scale), header_text, header_scale);
        std::string network = fit_text(font, presence.empty() ? (net_ok ? net.status() : net.error()) : presence, 300, header_scale);
        draw_text(font, 480-rich_text_width(font, header_scale, network.c_str())/2, ui_layout::header_baseline, network, header_text, header_scale);
        const std::string charge = battery_percent >= 0 && battery_percent <= 100 ? std::to_string(battery_percent) + "%" : "--%";
        // Lay out from the right edge: battery + percent | bold clock, with 8 px separator gaps.
        const int clock_x = 948-rich_text_width(font, header_scale, header_clock)-1;
        const int separator_x = clock_x-8-rich_text_width(font, header_scale, "|");
        const int charge_x = separator_x-8-rich_text_width(font, header_scale, charge.c_str());
        const int battery_x = charge_x-26;
        const bool battery_drawn = ui_battery::draw(battery_percent, battery_charging, battery_x, 8);
        if (!battery_drawn && battery_charging) draw_text(font, battery_x, ui_layout::header_baseline, "+", header_text, header_scale);
        draw_text(font, charge_x, ui_layout::header_baseline, charge, header_text, header_scale);
        draw_text(font, separator_x, ui_layout::header_baseline, "|", header_text, header_scale);
        // Synthetic bold adds one pixel to the strokes without changing the font size.
        draw_text(font, clock_x, ui_layout::header_baseline, header_clock, header_text, header_scale);
        draw_text(font, clock_x+1, ui_layout::header_baseline, header_clock, header_text, header_scale);
        // Keep the header fixed; translate only the content and its stencil clips below it.
        sceGxmSetViewport(vita2d_get_context(), 480.f+drawer_draw_shift, 480.f, 272.f, -272.f, 0.5f, 0.5f);
        if (settings_open && !theme_picker.active && !qr_scanner.active()) {
            vita2d_draw_line(28, 49, 35, 42, ink);
            vita2d_draw_line(28, 49, 35, 56, ink);
            draw_text(font, 44, 56, fit_text(font, tr("common.back"), 112, 0.5f), ink, 0.5f);
            draw_text(font, 184, 56, fit_text(font, heading, 748, 0.55f), ink, 0.55f);
        }
        std::string update_notice = tg.updateNotice();


        if (qr_scanner.active()) {
            if (qr_scanner.preview()) draw_texture_fit(qr_scanner.preview(), 240, 60, 480, 360);
            draw_text(font, 30, 462, fit_text(font, qr_scanner.status(), 900, 0.55f), ink, 0.55f);
            draw_text(font, 28, 528, tr("qr.footer"), ink, 0.6f);
        } else if (settings_open && folder_picker.active) {
            draw_text(font, 30, 68, fit_text(font, folder_picker.path.empty() ? tr("picker.drives") : folder_picker.path, 900, 0.64f), ink, 0.64f);
            draw_text(font, 32, 97, tr("picker.subtitle"), muted, 0.48f);
            skin_frame(24, 109, 912, 356, separator, white);
            int first = (folder_picker.selected / 8) * 8;
            for (int row = 0; row < 8 && first + row < (int)folder_picker.folders.size(); ++row) {
                int index = first + row;
                if (index == folder_picker.selected) skin_row(32, 117 + row * 43, 896, 40, true, selected, true);
                draw_text(font, 42, 144 + row * 43, fit_text(font, folder_picker.folders[index], 850, 0.65f), ink, 0.65f);
            }
            if (folder_picker.folders.empty() && folder_picker.error.empty())
                draw_text(font, 42, 144, tr("picker.empty"), muted, 0.55f);
            if (!folder_picker.error.empty()) draw_text(font, 30, 484, folder_picker.error, ink, 0.55f);
            draw_text(font, 28, 528, tr("picker.footer"), ink, 0.6f);
        } else if (settings_open && about_open) {
            const auto& layout = about_layout(font);
            about_offset = std::clamp(about_offset, 0, layout.maximum());
            skin_frame(24, 71, 912, 424, separator, white);
            vita2d_enable_clipping();
            vita2d_set_clip_rectangle(32, 76, 928, 490);
            for (const auto& line : layout.lines) {
                const int y = 76 + line.y - about_offset;
                if (y + 30 < 76 || y >= 490) continue;
                const unsigned int color = line.heading ? accent : ink;
                draw_text(font, 44, y + 24, line.text, color, line.scale);
                if (line.bold) draw_text(font, 45, y + 24, line.text, color, line.scale);
                if (line.heading && line.scale < 0.90f)
                    vita2d_draw_rectangle(44, y + 33, 856, 1, separator);
            }
            for (int i = 0; i < 2; ++i) {
                const int y = 76 + layout.link_y(i) - about_offset;
                if (y + 160 < 76 || y >= 490) continue;
                const std::string label = tr(i == 0 ? "about.project_link" : "about.author_link");
                draw_about_link_icon(i, 44, y + 6, accent);
                draw_text(font, 84, y + 27, label, accent, 0.68f);
                const bool focused = about_link == i && layout.link_visible(i, about_offset);
                if (focused) draw_text(font, 85, y + 27, label, accent, 0.68f);
                const int width = rich_text_width(font, 0.68f, label.c_str());
                vita2d_draw_rectangle(84, y + 32, width, focused ? 2 : 1, accent);
                draw_text(font, 84, y + 54, i == 0 ? kAboutProjectUrl : kAboutAuthorUrl, muted, 0.50f);
                if (i == 0) {
                    const auto& qr = about_project_qr();
                    if (qr.size) {
                        const int module = 4, side = (qr.size + 8) * module;
                        const int left = 900 - side, top = y;
                        vita2d_draw_rectangle(left, top, side, side, RGBA8(255,255,255,255));
                        for (int row = 0; row < qr.size; ++row)
                            for (int col = 0; col < qr.size; ++col)
                                if (qr.dark(col, row)) vita2d_draw_rectangle(left + (col + 4) * module, top + (row + 4) * module,
                                                                         module, module, RGBA8(0,0,0,255));
                    }
                }
            }
            vita2d_disable_clipping();
            if (layout.maximum() > 0) {
                const float thumb = std::max(24.f, 414.f * 414.f / layout.height);
                const float y = 76 + (414 - thumb) * about_offset / layout.maximum();
                if (!ui_skin::nine(ui_skin::Slice::ScrollThumb, 928, y, 4, thumb))
                    vita2d_draw_rectangle(928, y, 4, thumb, muted);
            }
            draw_text(font, 30, 511, fit_text(font, settings_error.empty() && about_chat_pending ? tg.updateNotice() : settings_error, 900, 0.45f), ink, 0.45f);
            draw_text(font, 28, 536, fit_text(font, tr("about.footer"), 904, 0.48f), ink, 0.48f);
        } else if (settings_open && logs_open) {
            const char* names[diag_log::kCount] = {"logs.performance", "logs.gif", "logs.upload", "logs.video", "logs.audio", "logs.vault"};
            for (int i = 0; i < diag_log::kCount; ++i) {
                skin_card(24, 104+i*36, 912, 32, i == settings_row, i == settings_row ? selected : white, true);
                // The status hugs the right edge; the description scrolls within its own column.
                const std::string status = fit_text(font, (settings.log_mask >> i) & 1u ? tr("common.enabled") : tr("common.disabled"), 300, 0.58f);
                const int status_width = rich_text_width(font, 0.58f, status.c_str());
                draw_text(font, 920 - status_width, 128+i*36, status, ink, 0.58f);
                draw_settings_text(font, settings_label_marquee, 200 + i, i == settings_row,
                    40, 104+i*36, 920 - status_width - 16 - 40, 32, 128+i*36,
                    tr(names[i]), ink, 0.62f, now);
            }
            draw_text(font, 30, 462, fit_text(font, tr("logs.note"), 900, 0.5f), muted, 0.5f);
            draw_text(font, 30, 484, settings_error, ink, 0.5f);
            draw_text(font, 28, 528, tr("logs.footer"), ink, 0.6f);
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
                    case PROXY_ROW_SERVER: return proxy_draft.proxy_server;
                    case PROXY_ROW_PORT: return std::to_string(proxy_draft.proxy_port);
                    case PROXY_ROW_SECRET: return proxy_draft.proxy_secret.empty() ? tr("common.not_set") : std::string("********");
                    case PROXY_ROW_USER: return proxy_draft.proxy_username.empty() ? tr("common.not_set") : proxy_draft.proxy_username;
                    case PROXY_ROW_PASS: return proxy_draft.proxy_password.empty() ? tr("common.not_set") : std::string("********");
                    case PROXY_ROW_SCAN: return tr("vnr.rear_camera");
                    default: return "";
                }
            };
            for (int i = 0; i < (int)rows.size(); ++i) {
                skin_card(24, 104+i*36, 912, 32, i == settings_row, i == settings_row ? selected : white, true);
                draw_settings_text(font, settings_label_marquee, 100 + rows[i], i == settings_row,
                    40, 104+i*36, 460, 32, 128+i*36, label(rows[i]), ink, 0.62f, now);
                draw_settings_text(font, settings_value_marquee, 100 + rows[i], i == settings_row,
                    520, 104+i*36, 400, 32, 128+i*36, value(rows[i]), ink, 0.58f, now);
            }
            // SOCKS5 and HTTP CONNECT carry the proxy login in clear text (Telegram data stays MTProto-encrypted).
            const bool cleartext_login = proxy_draft.proxy_type != 0 &&
                (!proxy_draft.proxy_username.empty() || !proxy_draft.proxy_password.empty());
            draw_text(font, 30, 438, proxy_imported ? tr("proxy.import_confirm") : cleartext_login ? tr("proxy.cleartext") : "", muted, 0.48f);
            draw_text(font, 30, 462, fit_text(font, tg.proxyCheckStatus().empty() ? tg.proxyStatus() : tg.proxyCheckStatus(), 900, 0.55f), muted, 0.55f);
            draw_text(font, 30, 484, settings_error, ink, 0.5f);
            draw_text(font, 28, 528, tr("proxy.footer"), ink, 0.6f);
        } else if (settings_open) {
            const std::string labels[] = {tr("settings.language"), tr("settings.retention"), tr("settings.cache_size"), tr("settings.theme"),
                tr("settings.font_size"), tr("settings.show_threads"), tr("settings.batch"), tr("settings.download_dir"), tr("proxy.title"),
                tr("settings.auto_update"), tr("settings.update"), tr("vault.settings.encryption"), tr("vault.settings.change_key"), tr("vault.settings.check"),
                tr("vault.settings.auto_lock"), tr("vault.settings.lock_on_leave"),
                tr("settings.gyroscope"), tr("settings.opacity"), tr("settings.avatar_shape"), tr("settings.show_archive"), tr("settings.logs"), tr("about.title"), tr("settings.recording_shortcuts"), tr("settings.spoiler_reset"), tr("settings.scroll_speed"), tr("settings.keyboard"), tr("settings.allow_vpk_install"), tr("settings.chat_open"), tr("settings.view_mode"), tr("settings.video_quality"), tr("settings.font_family"), tr("settings.logout"), tr("cache.clear")};
            std::string values[] = {
                language_label(),
                settings.retention == 4 ? tr("settings.always") : tr("settings.days", {std::to_string(AppSettings::days(settings.retention))}),
                settings.capacity == 5 ? tr("settings.no_limit") : tr("unit.mb", {std::to_string(AppSettings::megabytes(settings.capacity))}),
                theme_name(settings.theme), std::to_string(70+settings.font*10) + "%",
                settings.show_threads ? tr("common.yes") : tr("common.no"), std::to_string(settings.request_batch_size),
                settings.download_dir, settings.proxy_enabled ? tr("common.enabled") : tr("common.disabled"),
                !settings.check_updates ? tr("common.no") : settings.proxy_enabled ? tr("settings.auto_update_proxy") : tr("common.yes"),
                update_label(updater.snapshot()),
                confirm_vault_off ? tr("vault.settings.confirm_disable") :
                    vault::disabling() ? tr("vault.settings.disabling") :
                    vault::enabled() ? tr("common.enabled") : tr("common.disabled"),
                vault::enabled() && !vault::disabling() ? "" : "-",
                vault::enabled() && !vault::disabling() ? "" : "-",
                !vault::enabled() || vault::disabling() ? "-" : settings.auto_lock == 0 ? tr("common.no") :
                    tr("unit.min", {std::to_string(AppSettings::lockMinutes(settings.auto_lock))}),
                !vault::enabled() || vault::disabling() ? "-" : settings.lock_on_leave ? tr("common.yes") : tr("common.no"),
                settings.gyroscope_enabled ? tr("common.enabled") : tr("common.disabled"),
                std::to_string(settings.opacity) + "%", settings.avatar_shape == 1 ? tr("settings.avatar_round") : tr("settings.avatar_square"),
                settings.show_archive ? tr("common.yes") : tr("common.no"),
                settings.log_mask ? std::to_string(__builtin_popcount(settings.log_mask)) + "/" + std::to_string(diag_log::kCount) : tr("common.disabled"),
                "", settings.recording_shortcuts ? tr("common.enabled") : tr("common.disabled"),
                tr(settings.spoiler_reset == 1 ? "settings.spoiler_chat" : settings.spoiler_reset == 2 ? "settings.spoiler_app" :
                    settings.spoiler_reset == 3 ? "settings.spoiler_both" : "common.disabled"),
                tr(settings.scroll_speed == 1 ? "settings.scroll_fast" : settings.scroll_speed == 2 ? "settings.scroll_very_fast" : "settings.scroll_normal"),
                tr(settings.custom_keyboard ? "settings.keyboard_custom" : "settings.keyboard_system"),
                settings.allow_vpk_install ? tr("common.enabled") : tr("common.disabled"),
                tr(settings.chat_open_position == 1 ? "settings.chat_open_server" : settings.chat_open_position == 2 ? "settings.chat_open_device" : "settings.chat_open_latest"),
                tr(settings.combined_view ? "settings.view_combined" : "settings.view_classic"),
                tr(settings.video_quality == 1 ? "settings.video_360" : settings.video_quality == 2 ? "settings.video_480" :
                    settings.video_quality == 3 ? "settings.video_720" : "settings.video_minimum"),
                settings.font_file.empty() ? tr("settings.font_default") : settings.font_file,
                confirm_logout ? tr("settings.confirm_logout") : "",
                tg.cacheMaintenance() ? tr("cache.cleaning") : cache_cleanup_requested ?
                    (tg.cacheCleanupError().empty() ? tr("cache.done") : tr("cache.failed")) : ""};
            {
                settings_scroll.configure(keyboard_settings ? keyboard_rows() : settings_menu::counts[settings_section], kSettingsRowHeight, kSettingsViewport);
                // Same card geometry, opacity, focus bar and scrolling as the chat list.
                vita2d_enable_clipping();
                vita2d_set_clip_rectangle(20, kSettingsTop, 940, kSettingsTop + kSettingsViewport);
                int row_y = kSettingsTop + settings_scroll.first() * kSettingsRowHeight - settings_scroll.offset();
                for (int row = settings_scroll.first(); row < settings_scroll.end(); ++row, row_y += kSettingsRowHeight) {
                    skin_card(20, row_y + 1, 920, kSettingsRowHeight - 2, row == settings_row, row == settings_row ? selected : white);
                    const int source_row = settings_menu::rows[settings_section][keyboard_settings ? 0 : row];
                    std::string label = source_row < 0 ? tr(settings_menu::titles[-source_row]) :
                        source_row == 33 ? tr("settings.user_colors") : labels[source_row];
                    std::string value = source_row < 0 ? "" : source_row == 33 ?
                        tr(settings.user_colors ? "common.enabled" : "common.disabled") : values[source_row];
                    const bool disabled = !keyboard_settings && settings_menu::vaultDependent(source_row) &&
                        (!vault::enabled() || vault::disabling());
                    if (disabled) value = tr("vault.settings.not_enabled");
                    if (!keyboard_settings && source_row == 31)
                        vita2d_draw_rectangle(38, row_y + 1, 884, 1, separator);
                    if (keyboard_settings == 1) {
                        label = tr(row == 0 ? "settings.keyboard_type" : row == 1 ? "settings.keyboard_languages" : "settings.keyboard_split");
                        value = row == 0 ? tr(settings.custom_keyboard ? "settings.keyboard_custom" : "settings.keyboard_system") :
                            row == 1 ? input_language_summary() : tr(settings.keyboard_split == 0 ? "settings.keyboard_split_small" :
                                settings.keyboard_split == 1 ? "settings.keyboard_split_medium" : "settings.keyboard_split_wide");
                    } else if (keyboard_settings == 2) {
                        label = row == 0 ? tr("settings.keyboard_defaults") : input_language_name(keyboard_layout::all()[row - 1]);
                        value = row == 0 ? (settings.keyboard_languages.empty() ? tr("common.enabled") : "") :
                            tr(keyboard_languages::contains(keyboard_languages::resolve(settings.keyboard_languages, i18n::systemLanguage()),
                                keyboard_layout::all()[row - 1].code) ? "common.enabled" : "common.disabled");
                    }
                    draw_settings_text(font, settings_label_marquee, keyboard_settings ? 300 + keyboard_settings*30 + row : source_row, row == settings_row,
                        38, row_y + 1, source_row < 0 ? 856 : 460, kSettingsRowHeight - 2, row_y + 29, label, disabled ? muted : ink, 0.65f, now);
                    const bool theme_row = !keyboard_settings && source_row == 3;
                    const bool submenu_row = !keyboard_settings && settings_menu::submenu(source_row);
                    draw_settings_text(font, settings_value_marquee, keyboard_settings ? 300 + keyboard_settings*30 + row : source_row, row == settings_row,
                        520, row_y + 1, theme_row || submenu_row ? 376 : 400, kSettingsRowHeight - 2, row_y + 29, value, disabled ? muted : ink, 0.6f, now);
                    if (submenu_row) {
                        vita2d_draw_line(910, row_y + 17, 917, row_y + 24, ink);
                        vita2d_draw_line(917, row_y + 24, 910, row_y + 31, ink);
                    }
                    if (theme_row) {
                        vita2d_draw_line(908, row_y + 18, 914, row_y + 24, ink);
                        vita2d_draw_line(914, row_y + 24, 920, row_y + 18, ink);
                    }
                }
                vita2d_disable_clipping();
                if (settings_scroll.maximum() > 0) {
                    const float thumb = std::max(28.0f, (float)kSettingsViewport * kSettingsViewport / (kSettingsViewport + settings_scroll.maximum()));
                    const float thumb_y = kSettingsTop + (kSettingsViewport - thumb) * settings_scroll.offset() / settings_scroll.maximum();
                    if (!ui_skin::nine(ui_skin::Slice::ScrollThumb, 948, thumb_y, 8, thumb)) vita2d_draw_rectangle(953, thumb_y, 3, thumb, accent);
                }
            }
            { const Updater::Snapshot u = updater.snapshot();
              draw_text(font, 30, 500, fit_text(font, !settings_error.empty() ? settings_error : settings_section == 4 && cache_cleanup_requested && !tg.cacheCleanupError().empty() ? tg.cacheCleanupError() : u.state == Updater::State::Failed ?
                  ((u.error == Updater::Error::Network || u.error == Updater::Error::NoRelease) ?
                      tr("update.err.github_detail", {u.detail}) : u.detail) :
                  settings_section == 4 && settings_row == 4 ? tr("settings.cache_cleanup_hint") : std::string(), 900, 0.5f), ink, 0.5f); }
            draw_text(font, 28, 528, tr("settings.footer"), ink, 0.6f);
        } else if (!net_ok) {
            draw_text(font, 30, 115, tr("net.init_failed"), ink, 0.85f);
        } else if (network_held) {
            draw_text(font, 30, 115, tr("network.held"), ink, 0.75f);
        } else if (!tg.lastError().empty() && !tg.ready()) {
            draw_text(font, 30, 115, clip(tg.lastError(), 100), ink, 0.75f);
        }

        history_view = chat_view::layout(combined_visible(), folder_tabs);
        auto draw_chat_list = [&]() {
            const auto& chats = tg.chats();
            if (history_view.combined) {
                vita2d_draw_rectangle(0, history_view.searchTop(), chat_view::sidebar, list_bottom-history_view.searchTop(),
                    ui_skin::background_color(dark ? RGBA8(7,26,52,255) : RGBA8(237,245,253,255)));
                skin_row(history_view.listLeft(), history_view.searchTop(), history_view.listWidth(), ui_layout::search_height, combined_list_focus && search_bar_focus, white, true);
                ui_skin::icon(ui_skin::Icon::Search, 18+history_view.listInset(), history_view.searchTop()+2, 22);
                draw_text(font, 47+history_view.listInset(), history_view.searchTop()+20, fit_text(font, tr("search.title"), history_view.listWidth()-47, 0.5f), muted, 0.5f);
                vita2d_draw_rectangle(chat_view::sidebar, history_view.searchTop(), 1, list_bottom-history_view.searchTop(), separator);
            } else search_bar(font, tr("search.title"), search_bar_focus ? ink : muted, search_bar_focus ? selected : white, search_bar_focus);
            if (folder_tabs && !history_view.combined) {
                int active = 0;
                for (int i = 0; i < (int)tg.folders().size(); ++i) if (tg.folders()[i].id == tg.selectedFolder()) active = i;
                if (shown_folder_id != tg.selectedFolder() || shown_folder_index != active) {
                    if (active < folder_scroll / kFolderStep) folder_scroll = active * kFolderStep;
                    if (active >= folder_scroll / kFolderStep + 5) folder_scroll = (active - 4) * kFolderStep;
                    shown_folder_id = tg.selectedFolder(); shown_folder_index = active;
                }
                folder_scroll = std::max(0, std::min(folder_scroll, folder_max_scroll));
                vita2d_enable_clipping();
                vita2d_set_clip_rectangle(10, history_view.folderTop()-2, 950, history_view.folderTop()+42);
                // Show whole tabs, including while a finger drags between positions.
                const int first_folder = folder_scroll / kFolderStep;
                for (int i = first_folder; i < std::min(first_folder + 5, (int)tg.folders().size()); ++i) {
                    int x = history_view.listLeft() + (i - first_folder) * kFolderStep;
                    const bool tab_skinned = ui_skin::nine(i == active ? ui_skin::Slice::TabActive : ui_skin::Slice::TabNormal,
                        x, history_view.folderTop(), ui_layout::folder_width, ui_layout::folder_height);
                    if (!tab_skinned) vita2d_draw_rectangle(x, history_view.folderTop(), 184, 39, i == active ? selected : white);
                    draw_text(font, x+10, history_view.folderTop()+25, fit_text(font, tg.folders()[i].title, 164, 0.65f), i == active && !tab_skinned ? accent : ink, 0.65f);
                    if (i == active && !(tab_skinned && ui_skin::indicator(x+24, history_view.folderTop()+34, 136, 4))) vita2d_draw_rectangle(x, history_view.folderTop()+37, 184, 3, accent);
                }
                vita2d_disable_clipping();
                if (first_folder > 0) ui_skin::edge_glow(false, 10, history_view.folderTop(), 39);
                if (first_folder + 5 < (int)tg.folders().size()) ui_skin::edge_glow(true, 950, history_view.folderTop(), 39);
            } else { shown_folder_id = -1; shown_folder_index = -1; }
            chat_scroll.configure((int)chats.size(), chat_row_height, list_bottom-chat_top);
            const int base = chat_scroll.first(), end = chat_scroll.end();
            int y = chat_top + 23 + base * chat_row_height - chat_scroll.offset();
            if (!decode_cooldown) for (int i = base; i < end; ++i) tg.ensureChatMedia((size_t)i);
            vita2d_enable_clipping();
            vita2d_set_clip_rectangle(history_view.listLeft(), chat_top, history_view.listLeft() + history_view.listWidth(), list_bottom);
            const int focus_row = search_bar_focus || (history_view.combined && !combined_list_focus) ? -1 : chat_index;
            for (int i = base; i < end; ++i) {
                const int row_y = y + (int)std::lround(chat_motion.offset((size_t)i, now));
                const bool opened = history_view.combined && chats[i].id == tg.currentChatId();
                const bool focused = i == focus_row;
                const auto row_slice = history_view.combined ? (opened ? ui_skin::Slice::TabActive : ui_skin::Slice::Row) :
                    (focused ? ui_skin::Slice::RowFocused : ui_skin::Slice::Row);
                if (!ui_skin::nine(row_slice, history_view.listLeft(), row_y - 23, history_view.listWidth(), chat_row_height - 2,
                        ui_skin::card_tint(focused || opened))) {
                    vita2d_draw_rectangle(history_view.listLeft(), row_y - 23, history_view.listWidth(), chat_row_height - 2, (history_view.combined ? opened : focused) ? selected : white);
                }
                if (focused && (history_view.combined || settings.theme == 2) && !ui_skin::nine(ui_skin::Slice::FocusRing, history_view.listLeft(), row_y-23, history_view.listWidth(), chat_row_height-2)) {
                    vita2d_draw_rectangle(history_view.listLeft(), row_y-23, history_view.listWidth(), 2, accent);
                    vita2d_draw_rectangle(history_view.listLeft(), row_y+chat_row_height-27, history_view.listWidth(), 2, accent);
                    vita2d_draw_rectangle(history_view.listLeft(), row_y-21, 2, chat_row_height-6, accent);
                    vita2d_draw_rectangle(history_view.listLeft()+history_view.listWidth()-2, row_y-21, 2, chat_row_height-6, accent);
                }
                vita2d_texture* avatar = textures.get(chats[i].avatar_path, kMaskAvatar);
                {
                    auto& shown = chat_avatar_shown[chats[i].id];
                    if (avatar) shown = chats[i].avatar_path;
                    // A removed photo (no file at all) falls back to the placeholder.
                    else if (!shown.empty() && (!chats[i].avatar_path.empty() || chats[i].avatar_file_id || chats[i].cached_metadata))
                        avatar = textures.find(shown, kMaskAvatar);
                }
                // The avatar and title are centred in the row, whatever height the layout gives it.
                const float row_top = (float)(row_y - 23), row_inner = (float)(chat_row_height - 2);
                const float avatar_size = std::min(30.f, row_inner - 12.f);
                const float avatar_inset = (row_inner - avatar_size) / 2;
                const float avatar_x = history_view.listLeft() + avatar_inset, avatar_y = row_top + avatar_inset;
                if (tg.chatIsDeleted(chats[i])) draw_deleted_avatar(avatar_x, avatar_y, avatar_size, settings.avatar_shape);
                else if (avatar) draw_texture_fit(avatar, avatar_x, avatar_y, avatar_size, avatar_size);
                else draw_avatar_placeholder(avatar_x, avatar_y, avatar_size, settings.user_colors ? peer_colors::rgba(tg.chatColors(chats[i], dark).rgb[0]) : RGBA8(126,176,211,255), settings.avatar_shape);
                const int title_left = history_view.combined ? 50+history_view.listInset() : ui_layout::chat_title_left;
                const float title_scale = history_view.combined ? 0.50f : 0.72f;
                if (opened) vita2d_draw_rectangle(history_view.listLeft()+2, row_top+5, 3, row_inner-10, accent);
                draw_text(font, title_left, (int)(row_top + row_inner / 2) + 7,
                    fit_text(font, tg.chatTitle(chats[i]), history_view.listLeft() + history_view.listWidth() - title_left - 30, title_scale), ink, title_scale);
                if (chats[i].unread_count > 0 || chats[i].marked_unread) {
                    // Equidistant from the top, bottom and right edges of the row.
                    const float radius = 6.0f, row_h = (float)(chat_row_height - 2);
                    const float cx = history_view.listLeft() + history_view.listWidth() - row_h / 2.0f, cy = (float)(row_y - 23) + row_h / 2.0f;
                    const unsigned int notification = ui_skin::color("notification", RGBA8(30,144,255,255));
                    for (int g = 4; g >= 1; --g)
                        vita2d_draw_fill_circle(cx, cy, radius + g * 2.0f, (notification & 0x00FFFFFFu) | ((unsigned)(14 + (5 - g) * 4) << 24));
                    vita2d_draw_fill_circle(cx, cy, radius, notification);
                }
                y += chat_row_height;
            }
            vita2d_disable_clipping();
            if (chats.empty()) draw_text(font, 18+history_view.listInset(), chat_top+35, fit_text(font, tg.chatsLoading() ? tr("chats.loading_first") :
                (tg.chatsEnd() ? tr("chats.none") : tr("chats.load_failed")), history_view.listWidth()-16, history_view.combined ? 0.48f : 0.75f), muted, history_view.combined ? 0.48f : 0.75f);
            skin_strip(selected);
            // Rows from the previous launch stay on screen while Telegram confirms them: a quiet
            // "refreshing" line instead of a loading counter and a running bar.
            const bool startup_rows = tg.chatsFromStartupIndex() && !chats.empty();
            std::string progress = startup_rows ? tr("notice.refreshing_chats") :
                tr("chats.status", {tg.chatsLoading() ? tr("chats.state_loading") : tr("chats.state_ready"),
                std::to_string(chats.size()), tg.chatsEnd() ? tr("chats.all_loaded") : tr("chats.tap_more")});
            if (!startup_rows && !tg.chatsLoading() && tg.chatsLastUpdated() > 0)
                progress += " | " + tr("chats.last_updated", {chat_update_time(tg.chatsLastUpdated())});
            const Updater::Snapshot update_state = updater.snapshot();
            const std::string update_hint = update_state.state == Updater::State::Available ? tr("update.hint", {update_state.version}) + "   " : "";
            draw_text(font, 30, 58, !update_notice.empty() ? fit_text(font, update_notice, 900, 0.48f) :
                fit_text(font, update_hint + (tg.chatsError().empty() ? progress : tr("chats.retry", {tg.chatsError()})), 900, 0.48f), ink, 0.48f);
            if (tg.chatsLoading() && !startup_rows) {
                float position = (float)((now / 2000) % 840);
                if (!ui_skin::indicator(position + 20, 61, 120, 3)) vita2d_draw_rectangle(position + 20, 62, 120, 2, accent);
            }
        };
        // Settings own the content area and footer; never draw a second screen over them.
        draw_header.finish();
        performance::SlowScope draw_chat(performance::SlowPhase::Ui,"draw_chat");
        if (!settings_open && !tg.topicsOpen() && (history_view.combined || (!recorder.active() && !stickers_open && !gifs_open))) {
        if (tg.currentChatId() != 0 && !tg.historyAnchorPending()) {
            const auto& msgs = tg.messages();
            if (video_fullscreen && message_index >= 0 && message_index < (int)msgs.size()) {
                vita2d_draw_rectangle(0, 0, 960, 544, ui_skin::background_color(dark ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255)));
                vita2d_texture* picture = media_animation.texture();
                if (!picture) picture = textures.get(msgs[message_index].media_path);
                if (picture) {
                    float w = vita2d_texture_get_width(picture), h = vita2d_texture_get_height(picture);
                    float scale = std::min(960.0f/w, 544.0f/h);
                    draw_texture_fit(picture, (960-w*scale)/2, (544-h*scale)/2, 960, 544);
                }
                const MessageItem& fullscreen_message = msgs[message_index];
                if (fullscreen_message.animation_error.empty() && media_animation.buffering()) draw_buffering_spinner(480, 272, now, accent);
                const MediaLoadView load = video_load_view(fullscreen_message, media_animation.readyForPlayback());
                if (fullscreen_message.animation_error.empty() && (load.downloading || (load.waiting && !media_animation.failedToOpen()))) {
                    const float y = load.controls ? 400.f : 240.f;
                    // Over a playing video the download card hides with the controls; before playback it stays.
                    draw_media_loading(font, 180, y, 600, load, now, accent, ink, muted, "", load.controls ? media_ui_alpha : 1.f);
                }
                const std::string error = !fullscreen_message.animation_error.empty() ? fullscreen_message.animation_error :
                    media_animation.failedToOpen() ? media_animation.status() : "";
                if (!error.empty()) draw_text(font, 60, 336, fit_text(font, error, 840, 0.65f), ink, 0.65f);
                if (fullscreen_message.video && load.controls) {
                    const VideoProgress progress = video_progress(fullscreen_message);
                    const ControlBar bar = draw_control_bar(font, 60, 544 - 68, 840, 44, progress.position_us, progress.duration_us,
                        media_animation.playing(), progress.seekable, progress.scrubbing ? progress.fraction : -1.f, accent, ink, muted, media_ui_alpha);
                    if (!seek_open && !chat_menu_open) { scrub_play = bar.play; if (progress.seekable) scrub_bar = bar.track; }
                }
            } else if (photo_open && message_index >= 0 && message_index < (int)msgs.size() &&
                photo_index < (int)msgs[message_index].photos.size()) {
                const PhotoItem& photo = msgs[message_index].photos[photo_index];
                // Same full-screen presentation as the video player: no header, the picture fills the screen.
                vita2d_draw_rectangle(0, 0, 960, 544, ui_skin::background_color(dark ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255)));
                vita2d_texture* image = textures.get(photo.full_path.empty() ? photo.thumbnail_path : photo.full_path, spoiler_locked(msgs[message_index]) ? kMaskBlur : kMaskNone);
                if (image) photo_view.draw(image);
                else {
                    const std::string loading = tr("photo.loading_full");
                    draw_text(font, 480 - rich_text_width(font, 0.75f, loading.c_str()) / 2, 276, loading, ink, 0.75f);
                }
                if (photo.full_path.empty() && image) {
                    const std::string loading = tr("photo.loading_larger");
                    draw_text(font, 480 - rich_text_width(font, 0.65f, loading.c_str()) / 2, 480, loading, ink, 0.65f);
                }
                if (media_ui_alpha > 0.01f && msgs[message_index].photos.size() > 1) {
                    char counter[32];
                    std::snprintf(counter, sizeof(counter), "%d / %d", photo_index + 1, (int)msgs[message_index].photos.size());
                    const int counter_x = 480 - rich_text_width(font, 0.65f, counter) / 2;
                    draw_text(font, counter_x + 1, 31, counter, scale_alpha(RGBA8(0, 0, 0, 200), media_ui_alpha), 0.65f);   // shadow keeps it readable on any photo
                    draw_text(font, counter_x, 30, counter, scale_alpha(RGBA8(255, 255, 255, 255), media_ui_alpha), 0.65f);
                }
            } else if (post_open && message_index >= 0 && message_index < (int)msgs.size() && msgs[message_index].poll.id) {
                const auto& m = msgs[message_index]; const auto& poll = m.poll;
                draw_post_date(font, m, 0, selected, muted);
                auto question = wrap_text(font, poll.question, history_view.width(880));
                for (size_t i=0; i<question.size() && i<2; ++i) draw_text(font, history_view.x(32), history_view.y(ui_layout::poll_question_baseline)+i*ui_layout::poll_question_pitch, question[i], ink, 0.68f);
                std::string kind = poll.quiz ? tr("poll.kind.quiz") : tr("poll.kind.poll");
                kind += poll.anonymous ? tr("poll.anonymous") : tr("poll.public");
                kind += poll.multiple ? tr("poll.multiple") : tr("poll.single");
                draw_text(font, history_view.x(32), history_view.y(ui_layout::poll_kind_baseline), fit_text(font, kind, history_view.width(890), 0.52f), muted, 0.52f);
                int first = poll_index / 5 * 5;
                for (int row=first; row<(int)poll.order.size() && row<first+5; ++row) {
                    int index = poll.order[row]; const auto& option = poll.options[index];
                    int y = history_view.y(ui_layout::poll_top) + (row-first)*ui_layout::poll_step;
                    skin_row(history_view.x(ui_layout::poll_left), y, history_view.width(ui_layout::poll_width), ui_layout::poll_height, row==poll_index, row==poll_index ? selected : separator, true);
                    bool checked = std::find(poll_choices.begin(), poll_choices.end(), index)!=poll_choices.end();
                    const unsigned int control_on = ui_skin::color("control_active", accent);
                    const unsigned int control_off = ui_skin::color("control_inactive", muted);
                    const float cx = history_view.x(52), cy = y + 21.f;
                    if (checked) {
                        vita2d_draw_fill_circle(cx, cy, 16, scale_alpha(control_on, 0.08f));
                        vita2d_draw_fill_circle(cx, cy, 13, scale_alpha(control_on, 0.16f));
                        vita2d_draw_fill_circle(cx, cy, 10, scale_alpha(control_on, 0.24f));
                        vita2d_draw_fill_circle(cx, cy, 6, control_on);
                        vita2d_draw_fill_circle(cx - 1.5f, cy - 2, 2, scale_alpha(ui_skin::color("highlight", RGBA8(255,255,255,255)), 210.f/255.f));
                    }
                    for (int a = 0; a < 32; ++a) {
                        const float angle = a * 6.2831853f / 32, next = (a + 1) * 6.2831853f / 32;
                        vita2d_draw_line(cx + 9 * std::cos(angle), cy + 9 * std::sin(angle),
                                         cx + 9 * std::cos(next), cy + 9 * std::sin(next), checked ? control_on : control_off);
                    }
                    std::string label = option.text;
                    if (option.chosen) label += tr("poll.your_answer");
                    if (option.correct) label += tr("poll.correct");
                    draw_text(font, history_view.x(76), y+22, fit_text(font, label, history_view.width(832), 0.57f), ink, 0.57f);
                    if (poll.results || poll.closed || poll.answered()) {
                        const std::string count = std::to_string(option.percent)+"% • "+std::to_string(option.votes);
                        const int count_x = history_view.x(ui_layout::poll_left) + history_view.width(ui_layout::poll_width) - 12 - rich_text_width(font, 0.48f, count.c_str());
                        vita2d_draw_rectangle(history_view.x(38), y+39, (float)std::max(0, count_x-history_view.x(38)-12)*option.percent/100, 4, accent);
                        draw_text(font, count_x, y+44, count, muted, 0.48f);
                    }
                }
                std::string state = poll.closed ? tr("poll.state.closed") : poll.restricted ? tr("poll.state.restricted") :
                    (!poll.canVote() && poll.answered()) ? tr("poll.state.locked") : tr("poll.state.choose");
                std::string request = tg.pollStatus(m.id);
                if (!request.empty()) state = request;
                draw_text(font, history_view.x(32), history_view.y(467), fit_text(font, state+tr("poll.voters", {std::to_string(poll.voters)}), history_view.width(890), 0.53f), ink, 0.53f);
                if (!poll.explanation.empty()) draw_text(font, history_view.x(32), history_view.y(490), fit_text(font, poll.explanation, history_view.width(890), 0.5f), muted, 0.5f);
            } else if (post_open && message_index >= 0 && message_index < (int)msgs.size()) {
                const auto& message = msgs[message_index];
                auto lines = post_lines(font, message);
                post_text_scroll = std::max(0, std::min(post_text_scroll, post_scroll_max(message, lines.size())));
                vita2d_enable_clipping();
                vita2d_set_clip_rectangle(history_view.left(), history_view.combined ? history_view.top : ui_layout::history_clip_top, ui_layout::width, ui_layout::height);
                draw_post_date(font, message, post_text_scroll, selected, muted);
                int photo_y = post_photo_top()-post_text_scroll;
                if (photo_y+170 > 38) {
                    for (const auto& tile : photo_carousel(message, photo_index)) {
                        if (tile.x >= 960 || tile.x+tile.width <= history_view.left()) continue;
                        if (!decode_cooldown) tg.ensurePhotoThumbnail(message_index, tile.index);
                        skin_box(ui_skin::Slice::MediaFrame, tile.x-2, photo_y-2, tile.width+4, 174, tile.index == photo_index ? accent : separator, true);
                        auto texture = textures.get(message.photos[tile.index].thumbnail_path, spoiler_locked(message) ? kMaskBlur : kMaskNone);
                        if (texture) {
                            float w = vita2d_texture_get_width(texture), h = vita2d_texture_get_height(texture);
                            float scale = std::min(tile.width/w, 170.0f/h);
                            const float px = tile.x+(tile.width-w*scale)/2, py = photo_y+(170-h*scale)/2;
                            draw_texture_fit(texture, px, py, tile.width, 170);
                            if (spoiler_locked(message)) draw_spoiler_particles(px, py, w*scale, h*scale, now, message.id+tile.index);
                        } else if (spoiler_locked(message)) {
                            vita2d_draw_rectangle(tile.x, photo_y, tile.width, 170, RGBA8(40,48,58,255));
                            draw_spoiler_particles(tile.x, photo_y, tile.width, 170, now, message.id+tile.index);
                        } else draw_text(font, tile.x+12, photo_y+90, "…", muted, 0.65f);
                        if (tile.index == photo_index) ui_skin::nine(ui_skin::Slice::FocusRing, tile.x-2, photo_y-2, tile.width+4, 174);
                    }
                }
                if (!message.photos.empty()) {
                    std::string counter = std::to_string(photo_index+1)+" / "+std::to_string(message.photos.size());
                    int width = rich_text_width(font, 0.55f, counter.c_str());
                    int y = history_view.y(270+kPostShift)-post_text_scroll;
                    draw_text(font, history_view.center()-width/2, y, counter, muted, 0.55f);
                    // Draw navigation chevrons without relying on missing font glyphs.
                    int left = history_view.center()-20-width/2, right = history_view.center()+20+width/2;
                    vita2d_draw_line(left+5, y-13, left, y-8, muted);
                    vita2d_draw_line(left, y-8, left+5, y-3, muted);
                    vita2d_draw_line(right-5, y-13, right, y-8, muted);
                    vita2d_draw_line(right, y-8, right-5, y-3, muted);
                }
                if (message.animation || message.sticker) {
                    if (!decode_cooldown) tg.ensureMessageMedia((size_t)message_index);
                    int media_y = post_media_top()-post_text_scroll;
                    const int media_h = post_media_height_for(message);
                    const bool hidden_media = spoiler_locked(message);
                    const MediaLoadView load = video_load_view(message, !hidden_media && media_animation.readyForPlayback());
                    const bool has_video_file = !hidden_media && message.video && load.controls && message.animation_error.empty();
                    const bool media_available = message.animation_file_id > 0 || !message.animation_path.empty();
                    const bool load_card = !hidden_media && message.video && media_available && message.animation_error.empty() &&
                        (load.downloading || (load.waiting && !media_animation.failedToOpen() && (!unsupported_video || prepare_video)));
                    const VideoProgress video_state = video_progress(message);
                    if (media_y+media_h > 38 && media_y < 544) {
                        vita2d_texture* picture = message.animation && !hidden_media ? media_animation.texture() : nullptr;
                        if (!picture && !message.video_note) picture = textures.get(message.media_path, hidden_media ? kMaskBlur : kMaskNone);
                        if (!picture) picture = textures.get(message.media_fallback, hidden_media ? kMaskBlur : kMaskNone);
                        if (!picture && hidden_media) {
                            vita2d_draw_rectangle(history_view.x(180), media_y, history_view.width(600), media_h, RGBA8(40,48,58,255));
                            draw_spoiler_particles(history_view.x(180), media_y, history_view.width(600), media_h, now, message.id);
                        }
                        if (picture) {
                            float w = vita2d_texture_get_width(picture), h = vita2d_texture_get_height(picture);
                            float scale = std::min((float)history_view.width(900)/w, (float)media_h/h);
                            const float picture_x = history_view.x(30)+(history_view.width(900)-w*scale)/2, picture_y = media_y+(media_h-h*scale)/2;
                            if (message.video_note) {
                                // Round clip: no frame, a progress ring around it that can be dragged to seek.
                                const float side = 316.f, note_scale = std::min(side / w, side / h);
                                draw_texture_fit(picture, history_view.center() - w * note_scale / 2, media_y + (media_h - h * note_scale) / 2, side, side);
                                if (has_video_file) {
                                    const float cx = history_view.center(), cy = media_y + media_h / 2.f, radius = side / 2 + 11;
                                    draw_progress_ring(cx, cy, radius, 5, video_state.fraction, video_state.seekable,
                                        scale_alpha(muted, 0.5f), accent);
                                    if (video_state.seekable) scrub_ring = {true, cx, cy, radius, 0};
                                    if (!media_animation.playing()) {   // paused: a play mark over the picture, never covering the ring
                                        vita2d_draw_fill_circle(cx, cy, 34, RGBA8(0, 0, 0, 110));
                                        if (!ui_skin::icon(ui_skin::Icon::Play, cx - 24, cy - 24, 48, RGBA8(255, 255, 255, 230)))
                                            for (int row = 0; row < 32; ++row)
                                                vita2d_draw_rectangle(cx - 10, cy - 16 + row, 26 * (1.f - std::fabs(row - 15.5f) / 16.f), 1, RGBA8(255, 255, 255, 230));
                                    }
                                }
                            } else {
                                // The frame hugs the picture; stickers are shown without one.
                                if (!message.sticker)
                                    ui_skin::nine(ui_skin::Slice::MediaFrame, picture_x-4, picture_y-4, w*scale+8, h*scale+8);
                                draw_texture_fit(picture, picture_x, picture_y, history_view.width(900), media_h);
                                if (hidden_media) draw_spoiler_particles(picture_x, picture_y, w*scale, h*scale, now, message.id);
                                // Video corners follow the frame's radius (frame radius 9 minus its 4 px padding).
                                if (!message.sticker) ui_skin::corners(picture_x, picture_y, w*scale, h*scale, 6);
                            }
                        }
                    }
                    if (message.animation && !hidden_media && message.animation_error.empty() && media_animation.buffering() &&
                        media_y + media_h / 2 > 80 && media_y + media_h / 2 < 500)
                        draw_buffering_spinner(history_view.center(), media_y + media_h / 2.f, now, accent);
                    if (load_card && media_y + media_h > 71 && media_y < 500) {
                        const float y = load.controls ? media_y + media_h + 48 : media_y + (media_h - 64) / 2.f;
                        draw_media_loading(font, history_view.x(load.controls ? 30 : 180), y, history_view.width(load.controls ? 900 : 600),
                            load, now, accent, ink, muted, prepare_video ? media_animation.status() : "");
                    }
                    std::string state = message.animation_error;
                    if (state.empty() && message.video) {
                        if (!media_available) state = tr("media.unavailable");
                        else if (unsupported_video && !prepare_video && media_animation.preparationRequired()) state = tr("video.unsupported_hint");
                        else if (!load_card || media_animation.failedToOpen()) state = media_animation.status();
                    } else if (state.empty() && message.animation_path.empty() && !animation_stream && message.animation) {
                        if (message.animation_file_id <= 0) state = tr("media.unavailable");
                        else if (message.animation_size > 0)
                            state = tr("media.loading", {std::to_string(std::min<int64_t>(99,
                                message.animation_downloaded * 100 / message.animation_size)) + "%"});
                        else state = tr("media.loading", {media_byte_count(message.animation_downloaded)});
                    } else if (state.empty() && message.animation) state =
                        message.video && !message.video_note && ((message.video_width > 0 && message.video_height > 0 &&
                        !VideoQuality::hardwareRasterSupported(message.video_width, message.video_height)) ||
                        (tg.currentChatId() == raster_mismatch_chat && message.id == raster_mismatch_message))
                        ? (media_animation.preparationRequired() ? tr("video.unsupported_hint") : media_animation.status()) : media_animation.status();
                    if (hidden_media) state = tr("spoiler.footer");
                    const bool video_bar = has_video_file;
                    if (video_bar) {
                        // Controls are in the START menu; the player no longer prints its play/pause hint.
                        if (state == tr("anim.play_pause") || state == tr("anim.video_playing_hint") || state == tr("anim.play") || state == tr("anim.video_playing"))
                            state.clear();
                        if (message.video_note) {
                            const std::string timer = clock_text(video_state.position_us) + " / " +
                                (video_state.duration_us > 0 ? clock_text(video_state.duration_us) : std::string("--:--"));
                            state = state.empty() ? timer : timer + "  " + state;
                        } else {
                            // Control bar under the picture: icon, track and time share one row.
                            const ControlBar bar = draw_control_bar(font, history_view.x(30), media_y+media_h+8, history_view.width(900), 34, video_state.position_us, video_state.duration_us,
                                media_animation.playing(), video_state.seekable, video_state.scrubbing ? video_state.fraction : -1.f, accent, ink, muted);
                            scrub_play = bar.play;
                            if (video_state.seekable) scrub_bar = bar.track;
                        }
                    }
                    const int state_offset = video_bar ? (message.video_note ? 22 + scaled_px(14) : 60) : 23;
                    if (!state.empty()) draw_text(font, history_view.x(30), media_y + media_h + state_offset + (load_card && load.controls && !message.video_note ? 72 : 0),
                        fit_text(font, state, history_view.width(900), 0.55f), muted, 0.55f);
                }
                if (message.voice) {
                    skin_box(ui_skin::Slice::Panel, history_view.x(160), history_view.y(90)-post_text_scroll, history_view.width(640), 210, selected);
                    std::string state = message.voice_error;
                    if (state.empty() && message.voice_path.empty() && !voice_stream) {
                        if (!message.voice_file_id) state = tr("audio.unavailable");
                        else if (message.voice_size > 0) state = tr("audio.loading", {std::to_string(std::min<int64_t>(99, message.voice_downloaded * 100 / message.voice_size)) + "%"});
                        else state = tr("audio.loading", {media_byte_count(message.voice_downloaded)});
                    } else if (state.empty()) state = voice.status();
                    draw_text(font, history_view.x(184), history_view.y(130)-post_text_scroll, fit_text(font, message.audio ? message.audio_title : i18n::Text(tr("audio.voice_message")), history_view.width(590), 0.8f), ink, 0.8f);
                    if (message.audio) draw_text(font, history_view.x(184), history_view.y(157)-post_text_scroll, fit_text(font, message.audio_performer, history_view.width(590), 0.6f), muted, 0.6f);
                    draw_text(font, history_view.x(184), history_view.y(190)-post_text_scroll, fit_text(font, state, history_view.width(590), 0.7f), accent, 0.7f);
                    const int duration = message.voice_duration;
                    const bool voice_seekable = !message.voice_path.empty() && duration > 0;
                    const bool scrubbing_voice = scrub_active && scrub_kind == 1 && voice_seekable;
                    const double position = voice.positionSeconds();
                    float fraction = duration > 0 ? (float)std::max(0.0, std::min(1.0, position / duration)) : 0.f;
                    if (scrubbing_voice) fraction = scrub_fraction;
                    const int elapsed = scrubbing_voice ? (int)(fraction * duration) : (int)position;
                    char time[64]; std::snprintf(time, sizeof(time), "%d:%02d / %d:%02d", elapsed / 60, elapsed % 60, duration / 60, duration % 60);
                    draw_text(font, history_view.x(184), history_view.y(237)-post_text_scroll, time, muted, 0.7f);
                    if (message.waveform.size() >= 8) draw_waveform(history_view.x(184), history_view.y(270)-post_text_scroll, history_view.width(592), 38, message.waveform, fraction, accent, scale_alpha(muted, 0.55f));
                    else skin_bar(history_view.x(184), history_view.y(ui_skin::ready() ? 261 : 263)-post_text_scroll, history_view.width(592), ui_skin::ready() ? 10 : 6, fraction, separator, accent);
                    if (voice_seekable && history_view.y(298)-post_text_scroll > history_view.top)
                        scrub_voice = {true, (float)history_view.x(176), (float)std::max(history_view.top, history_view.y(244)-post_text_scroll), (float)history_view.width(608),
                            (float)(history_view.y(298)-post_text_scroll-std::max(history_view.top, history_view.y(244)-post_text_scroll))};
                }
                int text_y = post_text_top(message)-post_text_scroll;
                const int transcript_rows = count_transcript_rows(lines);
                const float effective_rows = (float)lines.size() - transcript_rows * (1.f - tr_reveal);
                const bool text_rows_shown = effective_rows > 0.05f;
                if (post_has_text(message) && (text_rows_shown || post_quote_extra(message) > 0)) {
                    // Bubble-style frame that grows with the text; the page scrolls as a whole.
                    const int panel_top = post_panel_top(message) - post_text_scroll;
                    const int panel_height = post_quote_extra(message) + (text_rows_shown ?
                        10 + scaled_px(18) + (int)std::lround(std::max(0.f, effective_rows - 1) * post_pitch()) + kPanelBottomPad : 6);
                    if (!ui_skin::nine(ui_skin::Slice::Panel, history_view.x(24), panel_top, history_view.width(912), panel_height, ui_skin::card_tint(false)))
                        { rounded_rect(history_view.x(24), panel_top, history_view.width(912), panel_height, separator); rounded_rect(history_view.x(26), panel_top + 2, history_view.width(908), panel_height - 4, white); }
                    const int header_top = panel_top + 8;
                    if (message.forwarded) draw_forward_header(font, history_view.x(38), (float)header_top, history_view.width(884), message, muted, settings.user_colors && message.forward_origin_id ? peer_colors::rgba(tg.senderColors(message.forward_origin_id, message.forward_from_chat, dark).rgb[0]) : accent);
                    if (message.reply_to_id) {   // same component as in the chat; tapping it opens the original
                        const float quote_top = (float)header_top + (message.forwarded ? forward_height() : 0);
                        draw_reply_quote(font, history_view.x(36), quote_top, history_view.width(888), message, dark, ink, accent, settings.user_colors && message.reply_sender_id ? tg.senderColors(message.reply_sender_id, message.reply_sender_chat, dark) : peer_colors::Palette{});
                        reply_hit = {true, (float)history_view.x(36), quote_top, (float)history_view.width(888), (float)quote_height()};
                    }
                }
                for (int line=0; line<(int)lines.size(); ++line) {
                    int baseline = text_y+(int)std::lround(reveal_row(line, transcript_rows, tr_reveal) * post_pitch());
                    if (baseline < 38) continue;
                    if (baseline-22 >= 544) break;
                    const float fade = lines[line].transcript ? tr_reveal : 1.f;
                    draw_post_line(font, history_view.x(38), baseline, lines[line], scale_alpha(ink, fade), scale_alpha(accent, fade), tg, textures, decode_cooldown, spoiler_visibility.revealed(tg.currentChatId(), message.id));
                }
                if (!message.preview_url.empty()) {
                    if (!decode_cooldown) tg.ensureMessageMedia((size_t)message_index);
                    draw_link_preview(font, message, textures, history_view.x(38), post_preview_top(message, lines.size()) - post_text_scroll, history_view.width(884), ink, settings.user_colors ? peer_colors::rgba(tg.senderColors(message.sender_id, message.sender_chat, dark).rgb[0]) : accent);
                }
                vita2d_disable_clipping();
                int maximum = post_scroll_max(message, lines.size());
                if (maximum > 0) {
                    const float track = history_view.combined ? 544-history_view.top : 482;
                    const float track_top = history_view.combined ? history_view.top : 62;
                    float thumb = std::max(28.0f, track*track/(track+maximum));
                    const float thumb_y = track_top+(track-thumb)*post_text_scroll/maximum;
                    if (!ui_skin::nine(ui_skin::Slice::ScrollThumb, 948, thumb_y, 8, thumb))
                        vita2d_draw_rectangle(953, thumb_y, 3, thumb, accent);
                }
            } else {
                bot_message_hits.clear();
                auto history_rows = history_layout(font, tg, message_index);
                const auto date_chip = history_date_chip(font, history_rows);
                vita2d_enable_clipping();
                vita2d_set_clip_rectangle(history_view.left(), history_view.top, 960, history_bottom);
                for (const auto& entry : history_rows) {
                    int i = entry.index;
                    if (!decode_cooldown) tg.ensureMessageMedia((size_t)i);
                    if (entry.day_top >= 0 && entry.index != date_chip.index) {
                        int width = rich_text_width(font, 0.55f, entry.day.c_str()) + 24;
                        skin_box(ui_skin::Slice::DateChip, history_view.center()-width/2, entry.day_top, width, 22, selected, true);
                        draw_text(font, history_view.center()+12-width/2, entry.day_top+16, entry.day, muted, 0.55f);
                    }
                    int y = entry.top + 4;
                    const MessageItem& m = msgs[i];
                    if (m.service_message) {
                        std::string label = fit_text(font, tg.serviceMessageText(m), 960-history_view.left()-100, 0.55f);
                        int width = rich_text_width(font, 0.55f, label.c_str()) + 28;
                        int left = history_view.center()-width/2;
                        if (ui_skin::nine(ui_skin::Slice::DateChip, left, y, width, 24)) {
                            if (i == message_index) ui_skin::nine(ui_skin::Slice::FocusRing, left-2, y-2, width+4, 28);
                        } else {
                            if (i == message_index) rounded_rect(left-2, y-2, width+4, 28, accent);
                            rounded_rect(left, y, width, 24,
                                dark ? RGBA8(35,57,76,255) : RGBA8(185,214,235,255));
                        }
                        draw_text(font, left+14, y+17, label, ink, 0.55f);
                        continue;
                    }
                    bool show_sender = tg.currentChatIsGroup() && !m.outgoing && !entry.before;
                    std::string sender = show_sender ? fit_text(font, tg.senderName(m), history_view.bubbleWidth()-58, 0.55f) : "";
                    bool selected_message = i == message_index;
                    std::string preview_path = m.document || !m.preview_url.empty() ? "" : !m.photos.empty() ? m.photos.front().thumbnail_path : m.media_path;
                    const bool spoiler_hidden = spoiler_locked(m);
                    vita2d_texture* media = textures.get(preview_path, spoiler_hidden ? kMaskBlur : m.video_note ? kMaskRoundThumb : kMaskNone);
                    if (!media) media = textures.get(m.media_fallback, spoiler_hidden ? kMaskBlur : kMaskNone);
                    bool has_media = message_has_media(m);
                    int media_width = has_media ? 108 : 0;
                    int max_text_width = history_view.bodyWidth() - media_width - (has_block_quote(m) ? 36 : 0);
                    auto lines = wrap_text(font, clip(m.text, 2048), max_text_width);
                    if (lines.size() > 2) {
                        lines.resize(2);
                        lines.back() = fit_text(font, lines.back() + "...", max_text_width, 0.68f);
                    }
                    const std::string media_label = m.document && m.document_size > 0 ?
                        m.media_label + " · " + media_byte_count(m.document_size) : std::string(m.media_label);
                    int text_width = m.photos.size() > 1 ? 230 : (m.voice && !m.audio) ? 400 : 130;
                    for (const auto& line : lines) text_width = std::max(text_width, rich_text_width(font, 0.68f, line.c_str()) + (has_block_quote(m) ? 36 : 0));
                    text_width = std::max(text_width, rich_text_width(font, 0.52f, media_label.c_str()) + (m.document ? 17 : 0));
                    text_width = std::max(text_width, rich_text_width(font, 0.52f, message_time(m).c_str()) + (m.outgoing ? 30 : 0));
                    int bubble_width = std::min(history_view.bubbleWidth(), text_width + media_width + 32);
                    if (!m.preview_url.empty()) bubble_width = std::max(400, bubble_width);
                    if (!m.buttons.empty()) bubble_width = std::max(600, bubble_width);
                    if (m.has_comments)   // room for the badge left of the timestamp
                        bubble_width = std::max(bubble_width, std::min(712, comments_badge_width(font, m) + rich_text_width(font, 0.52f, message_time(m).c_str()) + (m.outgoing ? 46 : 16) + 40));
                    if (show_sender) bubble_width = std::max(bubble_width, rich_text_width(font, 0.55f, sender.c_str()) + 58);
                    if (m.forwarded) {
                        const int header_width = rich_text_width(font, 0.45f, tr("forward.from").c_str()) + rich_text_width(font, 0.5f, (m.forward_source.empty() ? tr("forward.unknown") : m.forward_source).c_str()) + 44;
                        bubble_width = std::max(bubble_width, std::min(480, header_width));
                    }
                    if (m.reply_to_id) {   // the quote panel gets a readable width, never wider than the card allows
                        const int quote_width = std::max(rich_text_width(font, 0.5f, m.reply_sender.c_str()), rich_text_width(font, 0.55f, m.reply_text.c_str())) + 54;
                        bubble_width = std::max(bubble_width, std::min(ui_layout::quote_max_width, quote_width));
                    }
                    bubble_width = std::min(bubble_width, history_view.bubbleWidth());
                    int bubble_x = m.outgoing ? ui_layout::bubble_right - bubble_width : history_view.bubbleLeft();
                    const unsigned int bubble_surface = ui_skin::surface_color(white);
                    unsigned int bubble_color = m.outgoing ? radial::blend(bubble_surface, accent, 0.22f) : bubble_surface;
                    bubble_color = (bubble_color & 0x00FFFFFFu) | (ui_skin::card_tint(false) & 0xFF000000u);
                    draw_chat_bubble(bubble_x, y-4, bubble_width, entry.height-entry.gap(),
                        m.outgoing, entry.before, entry.after, bubble_color, selected_message, accent);
                    if (!m.buttons.empty() && m.buttons.inline_keyboard) {
                        const int by = entry.top + entry.height - bot_buttons::preview_height(m.buttons);
                        const unsigned surface = ui_skin::surface_color(white) | 0xff000000u;
                        const unsigned label = ui_color::tone(ink, surface, surface, 4.5);
                        for (const auto& cell : bot_buttons::cells(m.buttons, by, 36, 0, 4, bubble_x + 8, bubble_width - 16)) {
                            glass_panel(cell.x, cell.y, cell.w, cell.h, surface, surface, 6);
                            const unsigned border = ui_color::tone(ui_skin::color("border", separator), surface, surface, 1.5);
                            vita2d_draw_rectangle(cell.x + 6, cell.y, cell.w - 12, 1, border);
                            vita2d_draw_rectangle(cell.x + 6, cell.y + cell.h - 1, cell.w - 12, 1, border);
                            draw_text(font, cell.x + 10, cell.y + 24,
                                fit_text(font, m.buttons.rows[cell.row][cell.column].label, cell.w - 20, 0.55f), label, 0.55f);
                            if (cell.y >= 70 && cell.y + cell.h <= 494)
                                bot_message_hits.push_back({m.id, cell.x, cell.y, cell.w, cell.h, cell.row, cell.column});
                        }
                        if (bot_buttons::size(m.buttons) > 4) {
                            const int more_y = entry.top + entry.height - 40;
                            glass_panel(bubble_x + 8, more_y, bubble_width - 16, 32, surface, surface, 6);
                            draw_text(font, bubble_x + 18, more_y + 23, fit_text(font, tr("bot.buttons") + " (" +
                                std::to_string(bot_buttons::size(m.buttons)) + ")", bubble_width - 36, 0.55f), label, 0.55f);
                            if (more_y >= 70 && more_y + 32 <= 494)
                                bot_message_hits.push_back({m.id, bubble_x + 8, more_y, bubble_width - 16, 32, -1, -1});
                        }
                    }
                    const int grow = history_pitch() - 22;
                    y += kCardPad;   // breathing room between the card border and avatar/name/text
                    if (show_sender) {
                        const unsigned int sender_color = settings.user_colors ?
                            peer_colors::rgba(tg.senderColors(m.sender_id, m.sender_chat, dark).rgb[0]) : accent;
                        vita2d_texture* sender_avatar = textures.get(tg.senderAvatar(m), kMaskAvatar);
                        if (tg.senderIsDeleted(m)) draw_deleted_avatar(bubble_x+14, y-3, 18, settings.avatar_shape);
                        else if (sender_avatar) draw_texture_fit(sender_avatar, bubble_x+14, y-3, 18, 18);
                        else draw_avatar_placeholder(bubble_x+14, y-3, 18, sender_color, settings.avatar_shape);
                        draw_text(font, bubble_x+40, y+13, sender, sender_color, 0.55f);
                        y += std::max(20, scaled_px(20));
                    }
                    if (m.forwarded) {   // origin first, then the reply quote and the text
                        draw_forward_header(font, (float)bubble_x + 14, (float)y - 2, (float)bubble_width - 28, m, muted, settings.user_colors && m.forward_origin_id ? peer_colors::rgba(tg.senderColors(m.forward_origin_id, m.forward_from_chat, dark).rgb[0]) : accent);
                        y += forward_height();
                    }
                    if (m.reply_to_id) {   // between the name and the new text, spaced like the rest of the card
                        draw_reply_quote(font, (float)bubble_x + 10, (float)y - 2, (float)bubble_width - 20, m, dark, ink, accent, settings.user_colors && m.reply_sender_id ? tg.senderColors(m.reply_sender_id, m.reply_sender_chat, dark) : peer_colors::Palette{});
                        reply_rects.push_back({m.id, {true, (float)bubble_x + 10, (float)y - 2, (float)bubble_width - 20, (float)quote_height()}});
                        y += quote_height() + ui_layout::quote_gap;
                    }
                    const int label_row = message_body_end(m, lines.size()), time_row = message_time_row(m, lines.size());
                    if (media) {
                        if (m.video_note) draw_texture_fit(media, (float)bubble_x+30, (float)y+4, 56, 56);   // square, masked to a circle on upload
                        else {
                            draw_texture_fit(media, (float)bubble_x+10, (float)y+4, 96, 56);
                            if (spoiler_hidden) {
                                draw_spoiler_particles(bubble_x+10, y+4, 96, 56, now, m.id);
                                const std::string hidden = tr("spoiler.hidden");
                                const int label_width = rich_text_width(font, 0.55f, hidden.c_str());
                                draw_text(font, bubble_x + 58 - label_width / 2 + 1, y + 37, hidden, RGBA8(0, 0, 0, 160), 0.55f);
                                draw_text(font, bubble_x + 58 - label_width / 2, y + 36, hidden, RGBA8(255, 255, 255, 255), 0.55f);
                            }
                        }
                    }
                    else if (spoiler_hidden && has_media) {
                        vita2d_draw_rectangle(bubble_x+10, y+4, 96, 56, RGBA8(40,48,58,255));
                        draw_spoiler_particles(bubble_x+10, y+4, 96, 56, now, m.id);
                    }
                    else if (has_media) draw_text(font, bubble_x+12, y+32, m.video_note ? tr("chat.thumb.round") : m.video ? tr("chat.thumb.video") : m.animation ? "GIF" : "...", muted, 0.68f);
                    if (m.voice && !m.audio) {
                        // On colored discs the glyph must contrast with the disc, independently of the icon role.
                        const bool current = voice_message && voice_message->id == m.id;
                        const bool playing = current && voice.playing();
                        const float disc_x = (float)bubble_x + 36, disc_y = (float)y + 33;
                        const unsigned glyph = ui_color::on(accent);
                        vita2d_draw_fill_circle(disc_x, disc_y, 23, accent);
                        if (playing) {
                            vita2d_draw_rectangle(disc_x - 7, disc_y - 8, 5, 16, glyph);
                            vita2d_draw_rectangle(disc_x + 2, disc_y - 8, 5, 16, glyph);
                        } else about_triangle(disc_x - 5, disc_y - 9, disc_x + 9, disc_y, disc_x - 5, disc_y + 9, glyph);
                        voice_play_rects.push_back({m.id, {true, (float)bubble_x + 10, (float)y + 8, 52, 52}});
                        const float wave_x = (float)bubble_x + 70, wave_w = (float)bubble_width - 70 - 16;
                        const float fraction = current && m.voice_duration > 0 ?
                            (float)std::max(0.0, std::min(1.0, voice.positionSeconds() / m.voice_duration)) : 0.f;
                        if (m.waveform.size() >= 8) draw_waveform(wave_x, (float)y + 33, wave_w, 40, m.waveform, fraction, accent, scale_alpha(muted, 0.55f));
                        else skin_bar(wave_x, (float)y + 29, wave_w, 8, fraction, separator, accent);
                        const int shown = current ? (int)voice.positionSeconds() : m.voice_duration;
                        char length[16]; std::snprintf(length, sizeof(length), "%d:%02d", shown / 60, shown % 60);
                        draw_text(font, bubble_x + bubble_width - 16 - rich_text_width(font, 0.52f, length), y + label_row, length, muted, 0.52f);
                    }
                    int text_x = bubble_x + 16 + media_width;
                    const auto styles = map_styles(m, lines);
                    std::vector<PostLine> formatted;
                    for (size_t row = 0; row < lines.size(); ++row) {
                        PostLine line(lines[row], ""); line.styles = styles[row]; formatted.push_back(std::move(line));
                    }
                    layout_block_quotes(font, m, lines, formatted, history_pitch());
                    for (size_t row = 0; row < formatted.size(); ++row) {
                        draw_formatted_line(font, text_x, y+20+grow+(int)row*history_pitch(), formatted[row], ink, accent,
                            spoiler_visibility.revealed(tg.currentChatId(), m.id));
                    }
                    if (!m.media_label.empty()) {
                        int label_x = text_x;
                        if (m.document) {
                            if (!ui_skin::icon(ui_skin::Icon::File, label_x-3, y+label_row-16, 16)) {
                                vita2d_draw_rectangle(label_x, y+label_row-13, 7, 3, muted);
                                vita2d_draw_rectangle(label_x, y+label_row-10, 10, 9, muted);
                            }
                            label_x += 15;
                        }
                        draw_text(font, label_x, y + label_row, fit_text(font, media_label, bubble_x+bubble_width-12-label_x, 0.52f), muted, 0.52f);
                    }
                    draw_link_preview(font, m, textures, bubble_x + 10, y + label_row - 4, bubble_width - 20, ink, settings.user_colors ? peer_colors::rgba(tg.senderColors(m.sender_id, m.sender_chat, dark).rgb[0]) : accent);
                    std::string timestamp = message_time(m);
                    int time_right = bubble_x + bubble_width - (m.outgoing ? 46 : 16);
                    draw_text(font, time_right - rich_text_width(font, 0.52f, timestamp.c_str()), y+time_row, timestamp, muted, 0.52f);
                    if (m.outgoing) draw_delivery((float)(bubble_x+bubble_width-41), (float)y+time_row-11,
                                                  m.delivery, bubble_color | 0xFF000000u, now);
                    if (m.has_comments) {
                        const int badge_right = time_right - rich_text_width(font, 0.52f, timestamp.c_str()) - 16;
                        const int badge_left = draw_comments_badge(font, badge_right, y + time_row, m, ink, accent);
                        comments_rects.push_back({m.id, {true, (float)badge_left - 10, (float)(y + time_row - 18), (float)(badge_right - badge_left + 20), 26}});
                    }
                }
                if (date_chip.index >= 0) {
                    skin_box(ui_skin::Slice::DateChip, date_chip.x, date_chip.y, date_chip.width, 22, selected, true);
                    draw_text(font, date_chip.x+12, date_chip.y+16, message_day(msgs[date_chip.index]), muted, 0.55f);
                }
                vita2d_disable_clipping();
                if (msgs.empty()) draw_text(font, history_view.left()+38, history_view.top+60, tr("chat.no_messages"), muted, 0.75f);
                if (!bot_panel && tg.botWebApp().phase == TelegramClient::BotWebApp::Closed && bot_chat && chat_ui_visible()) {
                    const unsigned surface = ui_skin::surface_color(white) | 0xff000000u;
                    const unsigned label = ui_color::tone(ink, surface, surface, 4.5);
                    if ((!bg_show && history_bottom == 494) || (bg_show && bg_bar.bottom == 494)) {
                        vita2d_draw_rectangle(history_view.left(), 494, 960-history_view.left(), 50, surface);
                        vita2d_draw_rectangle(history_view.left(), 494, 960-history_view.left(), 1, separator);
                    }
                    if (!tg.botMenu().empty()) {
                        const auto control = bot_geometry.cell(bot_buttons::menu_control);
                        glass_panel(control.x, control.y, control.w, control.h, surface, surface, 7);
                        draw_text(font, bot_geometry.x(94), 527, fit_text(font, bot_buttons::menu_app(tg.botMenu()) ? tg.botMenu().rows[0][0].label : tr("bot.menu"), bot_geometry.width(182), 0.62f), label, 0.62f);
                    }
                    if (!tg.botKeyboard().buttons.empty()) {
                        const auto control = bot_geometry.cell(bot_buttons::keyboard_control);
                        glass_panel(control.x, control.y, control.w, control.h, surface, surface, 7);
                        draw_text(font, bot_geometry.x(319), 527, fit_text(font, tr("bot.keyboard"), bot_geometry.width(257), 0.62f), label, 0.62f);
                    }
                    if (!tg.botMenu().empty() || !tg.botKeyboard().buttons.empty() ||
                        (message_index >= 0 && message_index < (int)tg.messages().size() && !tg.messages()[message_index].buttons.empty()))
                        { glass_panel(bot_geometry.x(605), 498, bot_geometry.span(605, 280), 44, surface, surface, 7);
                          draw_text(font, bot_geometry.x(617), 526, fit_text(font, tr("bot.select_hint") + (message_index >= 0 && message_index < (int)tg.messages().size() && !tg.messages()[message_index].buttons.empty() ? " (" + std::to_string(bot_buttons::size(tg.messages()[message_index].buttons)) + ")" : ""), bot_geometry.width(256), 0.48f), label, 0.48f); }
                }
                if (!settings.combined_view && !chat_modal_open()) draw_attach_button(dark);
            }
        } else if (tg.canBrowse()) {
            if (!history_view.combined) draw_chat_list();
            if (history_view.combined && !tg.historyAnchorPending()) draw_text(font, chat_view::sidebar+24, chat_top+48,
                tr("chat.select_chat"), muted, 0.65f);
        } else if (!tg.qrLink().empty()) {
            const QrCode& qr = auth_qr;
            draw_text(font, 30, 70, tr("auth.qr_title"), ink, 0.8f);
            if (auth_qr_texture) {
                vita2d_draw_texture(auth_qr_texture, 30, 90);
            } else if (qr.size) {
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
            if (!tg.authDetail().empty() && (tg.inputKind() == InputKind::Code || tg.inputKind() == InputKind::EmailCode ||
                tg.inputKind() == InputKind::Phone))
                draw_text(font, 30, 222, fit_text(font, tg.authDetail(), 900, 0.62f), ink, 0.62f);
            if (!tg.authResendInfo().empty() && tg.inputKind() == InputKind::Code)
                draw_text(font, 30, 252, fit_text(font, tg.authResendInfo(), 900, 0.58f), muted, 0.58f);
            if (tg.canGoBackInput())
                draw_text(font, 30, 490, tg.canResendCode() ? tr("auth.code_hint") : tr("auth.back_hint"), muted, 0.6f);
            if (tg.canRequestQr()) {
                skin_row(510, 280, 420, 60, true, selected);
                draw_text(font, 528, 318, fit_text(font, tr("auth.qr_hint"), 385, 0.65f), ink, 0.65f);
            }
            skin_row(30, 280, 440, 60, true, selected);
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
        if (history_view.combined) {
            draw_chat_list();
        }
        draw_chat.finish();

        performance::SlowScope draw_overlays(performance::SlowPhase::Ui,"draw_overlays");
        if (!settings_open && !recorder.active() && !stickers_open && !gifs_open && !tg.topicsOpen() && !photo_open && !video_fullscreen && tg.currentChatId()) {
            // Status strip sits right under the header so it never covers media.
            skin_strip(selected);
            int progress = tg.historyPercent();
            if (!ui_skin::indicator(20, 61, 920.0f * progress / 100, 3)) vita2d_draw_rectangle(20, 62, 920.0f * progress / 100, 2, accent);
            std::string thread_info = settings.show_threads ? " | " + cpu_scheduler::threadStatus() : "";
            const std::string loading = tr(tg.historyLoading() ? "history.block_loading" : "history.block_ready",
                {std::to_string(progress)}) + thread_info;
            const std::string audio_status = tg.uploadStatus();
            std::string info = !update_notice.empty() ? update_notice : !tg.documentStatus().empty() ? tg.documentStatus() : !tg.transcriptionStatus().empty() ? tg.transcriptionStatus() :
                !browser_status.empty() ? browser_status :
                !tg.botStatus().empty() ? std::string(tg.botStatus()) :
                (!audio_status.empty() ? audio_status :
                (tg.historyError().empty() ? loading : tr("history.error", {tg.historyError()})));
            draw_text(font, 30, 58, fit_text(font, info, 904, 0.48f), ink, 0.48f);

        }
        if (!settings_open && !tg.currentChatId() && !tg.uploadStatus().empty()) {
            skin_strip(selected);
            draw_text(font, 30, 58, fit_text(font, tg.uploadStatus(), 900, 0.48f), ink, 0.48f);
        }
        if (!settings_open && !tg.ready() && tg.canBrowse() && !kb.active()) {
            vita2d_draw_rectangle(0, 78, 960, 26, RGBA8(20,35,50,240));
            draw_text(font, 20, 98, tr("diag.cached_waiting"), header_text, 0.55f);
        }
        auto tool_backdrop = [&](int top, int height) {
            if (!history_view.combined) { skin_backdrop(top, height, white); return; }
            vita2d_draw_rectangle(0, top, 960, height, RGBA8(0,0,0,140));
            skin_box(ui_skin::Slice::Dialog, chat_view::ToolWindow::left, top,
                chat_view::ToolWindow::width, height, ui_skin::surface_color(white) | 0xff000000u, true);
        };
        if (!settings_open && search_open && tg.canBrowse()) {
            tool_backdrop(36, 508);
            const auto results = tg.searchResults();
            const int count = (int)results.size();
            search_index = std::max(0, std::min(search_index, count - 1));
            search_first = std::max(0, std::min(search_index - kSearchRows / 2, count - kSearchRows));
            search_bar(font, tg.searchQuery().empty() ? tr("search.title") : tg.searchQuery(), tg.searchQuery().empty() ? muted : ink, selected);
            if (!decode_cooldown) for (int i = search_first; i < std::min(count, search_first + kSearchRows); ++i) tg.ensureSearchMedia(results[i].id);
            for (int i = search_first; i < std::min(count, search_first + kSearchRows); ++i) {
                const float y = (float)(kSearchTop + (i - search_first) * kSearchRow);
                skin_row(20, y, 920, kSearchRow - 4, i == search_index, i == search_index ? selected : white);
                vita2d_texture* avatar = textures.get(results[i].avatar_path, kMaskAvatar);
                if (tg.chatIsDeleted(results[i])) draw_deleted_avatar(32, y + 4, 32, settings.avatar_shape);
                else if (avatar) draw_texture_fit(avatar, 32, y + 4, 32, 32);
                else draw_avatar_placeholder(32, y + 4, 32, settings.user_colors ? peer_colors::rgba(tg.chatColors(results[i], dark).rgb[0]) : RGBA8(126,176,211,255), settings.avatar_shape);
                draw_text(font, 76, y + 27, fit_text(font, tg.chatTitle(results[i]), 640, 0.68f), ink, 0.68f);
                const char* kind = results[i].user_id ? "search.kind_user" : results[i].channel ? "search.kind_channel" :
                                   results[i].group ? "search.kind_group" : "search.kind_chat";
                draw_text(font, 800, y + 26, fit_text(font, tr(kind), 126, 0.5f), muted, 0.5f);
            }
            if (!count) draw_text(font, 38, kSearchTop + 30, tg.searchQuery().empty() ? tr("search.hint") :
                tg.searchPending() ? tr("search.searching") : tr("search.none"), muted, 0.7f);
            draw_text(font, 84, 526, fit_text(font, tr("search.footer"), 780, 0.55f), ink, 0.55f);
            skin_strip(selected);
            draw_text(font, 30, 58, tr("search.title"), ink, 0.48f);
            if (tg.searchPending()) {
                float position = (float)((now / 2000) % 840);
                if (!ui_skin::indicator(position + 20, 61, 120, 3)) vita2d_draw_rectangle(position + 20, 62, 120, 2, accent);
            }
        }
        if (!settings_open && reactions_open) {
            tool_backdrop(36, 508);
            draw_text(font, 32, 80, tr("reaction.title"), ink, 0.85f);
            if (tg.reactionStatus() != tr("reaction.hint"))
                draw_text(font, 32, 112, fit_text(font, tg.reactionStatus(), 880, 0.6f), muted, 0.6f);
            const auto& choices = tg.reactionChoices();
            const ReactionGrid grid{(int)choices.size(), tg.reactionTopCount()};
            const int begin = grid.page(reaction_index);
            const unsigned surface = ui_skin::surface_color(white);
            const unsigned focus = ui_color::tone(ui_skin::color("focus", accent), selected, surface);
            const unsigned normal_ink = ui_color::tone(ink, surface, surface, 4.5);
            const unsigned chosen_ink = ui_color::tone(ink, selected, selected, 4.5);
            const unsigned badge = ui_color::tone(accent, selected, selected);
            const unsigned check = ui_color::on(badge);
            for (int cell = begin; cell < begin + ReactionGrid::page_size; ++cell) {
                const int i = grid.index(cell);
                if (i < 0) continue;
                const int x = grid.x(cell), y = grid.y(cell);
                const auto state = tg.reactionState(i);
                const unsigned fill = state.chosen ? selected : surface;
                const unsigned cell_ink = state.chosen ? chosen_ink : normal_ink;
                // Focus is an outline; the persistent choice has a fill and a separate check badge.
                if (i == reaction_index) {
                    rounded_rect(x, y, grid.width, grid.height, focus);
                    rounded_rect(x + 2, y + 2, grid.width - 4, grid.height - 4, fill);
                } else rounded_rect(x, y, grid.width, grid.height, fill);
                if (!decode_cooldown) tg.ensureReactionMedia(i);
                auto icon = textures.get(tg.reactionIcon(i));
                if (icon) {
                    const float w = vita2d_texture_get_width(icon), h = vita2d_texture_get_height(icon);
                    const float scale = std::min(32.f / w, 32.f / h);
                    draw_texture_fit(icon, x + (grid.width - w * scale) / 2, y + 8 + (32 - h * scale) / 2, 32, 32);
                } else {
                    float scale = 1.f / settings.fontScale();
                    const int width = rich_text_width(font, scale, choices[i].c_str());
                    if (width > 56) scale *= 56.f / width;
                    const int size = (int)(32 * scale * settings.fontScale() + 0.5f);
                    draw_text(font, x + (grid.width - rich_text_width(font, scale, choices[i].c_str())) / 2,
                        y + 8 + (32 - size) / 2 + size - 3 * scale * settings.fontScale(), choices[i], cell_ink, scale);
                }
                if (state.count) {
                    const std::string count = fit_text(font, std::to_string(state.count), grid.width - 16, 0.45f);
                    draw_text(font, x + (grid.width - rich_text_width(font, 0.45f, count.c_str())) / 2, y + 58, count, cell_ink, 0.45f);
                }
                if (state.chosen) {
                    vita2d_draw_fill_circle(x + 90, y + 13, 8, badge);
                    for (int line = 0; line < 2; ++line) {
                        vita2d_draw_line(x + 86, y + 13 + line, x + 89, y + 16 + line, check);
                        vita2d_draw_line(x + 89, y + 16 + line, x + 94, y + 10 + line, check);
                    }
                }
            }
            skin_row(32, 444, 200, 44, false, white, true);
            draw_text(font, 48, 474, fit_text(font, tr("hint.back"), 168, 0.6f), ink, 0.6f);
            if (!choices.empty()) {
                const std::string pages = std::to_string(begin / grid.page_size + 1) + " / " +
                    std::to_string(grid.slot((int)choices.size() - 1) / grid.page_size + 1);
                draw_text(font, 920 - rich_text_width(font, 0.55f, pages.c_str()), 474, pages, ink, 0.55f);
            }
            draw_text(font, 32, 526, fit_text(font, tr("hint.reaction_footer"), 896, 0.5f), ink, 0.5f);
        }

        if (!settings_open && tg.topicsOpen()) {
            if (!history_view.combined) skin_backdrop(36, 508, white);
            else vita2d_draw_rectangle(history_view.left(), history_view.top, history_view.width(960), 544-history_view.top, ui_skin::background_color(white));
            vita2d_enable_clipping();
            vita2d_set_clip_rectangle(history_view.left(), history_view.combined ? history_view.top : 36, 960, 544);
            const auto& topics = tg.topics();
            const int rows = history_view.topicRows(), first = std::max(0, topic_index-rows+1);
            for (int i = first; i < std::min(first+rows, (int)topics.size()); ++i) {
                int y = history_view.topicTop()+(i-first)*50;
                skin_row(history_view.x(20), y, history_view.width(920), 48, i == topic_index, i == topic_index ? selected : white);
                std::string title = topics[i].title + (topics[i].closed ? tr("topics.closed") : "");
                draw_text(font, history_view.x(34), y+32, fit_text(font, title, history_view.width(740), 0.75f), ink, 0.75f);
                if (topics[i].unread) draw_text(font, 830, y+32, std::to_string(topics[i].unread), ui_skin::color("notification", accent), 0.7f);
            }
            draw_text(font, history_view.x(30), 492, fit_text(font, tg.topicsStatus(), history_view.width(900), 0.55f), muted, 0.55f);
            vita2d_disable_clipping();
            if (history_view.combined) vita2d_draw_rectangle(history_view.left(), 42, history_view.width(960), 23, ui_skin::surface_color(white));
            draw_text(font, history_view.x(30), history_view.combined ? 58 : 61, tr("topics.title"), ink, 0.7f);
        }
        if (!settings_open && stickers_open) {
            tool_backdrop(36, 508);
            const auto& packs = tg.stickerPacks();
            if (sticker_pack_menu) {
                draw_text(font, 30, 76, tr("sticker.sets_title"), ink, 0.8f);
                draw_text(font, 30, 108, fit_text(font, tg.stickerStatus(), 900, 0.55f), muted, 0.55f);
                int first = std::max(0, sticker_pack - 6);
                for (int i = first; i < std::min(first + 7, (int)packs.size()); ++i) {
                    int y = 116 + (i-first)*54;
                    skin_row(24, y, 912, 48, i == sticker_pack, i == sticker_pack ? selected : white);
                    if (!decode_cooldown) tg.ensureStickerPackIcon(i);
                    auto icon = textures.get(packs[i].icon_path);
                    if (icon) draw_texture_fit(icon, 30, y+4, 40, 40);
                    else { vita2d_draw_rectangle(30, y+6, 36, 36, separator); draw_text(font, 40, y+32, "S", ink, 0.6f); }
                    draw_text(font, 82, y+31, fit_text(font, packs[i].title, 820, 0.65f), ink, 0.65f);
                }
            } else {
                std::string title = tg.stickerSetOpen() ? tg.stickerSetTitle() : sticker_pack < (int)packs.size() ? packs[sticker_pack].title : i18n::Text(tr("sticker.title"));
                draw_text(font, 30, 76, fit_text(font, title, 900, 0.8f), ink, 0.8f);
                draw_text(font, 30, 112, fit_text(font, tg.stickerStatus(), 900, 0.6f), muted, 0.6f);
                const auto& items = tg.stickers();
                int first = (sticker_index/18)*18;
                for (int i = first; i < std::min(first+18, (int)items.size()); ++i) {
                    int x = 30+(i-first)%6*150, y = 130+(i-first)/6*110;
                    skin_row(x, y, 144, 104, i == sticker_index, i == sticker_index ? selected : white);
                    if (!decode_cooldown) tg.ensureSticker(i);
                    auto texture = textures.get(items[i].path);
                    if (texture) {
                        float w = vita2d_texture_get_width(texture), h = vita2d_texture_get_height(texture);
                        float scale = std::min(130.0f/w, 90.0f/h);
                        draw_texture_fit(texture, x+(144-w*scale)/2, y+(104-h*scale)/2, 130, 90);
                    } else draw_text(font, x+45, y+60, items[i].emoji.empty() ? "..." : items[i].emoji, ink, 0.85f);
                }
                if (tg.stickerSetOpen() && tg.stickerSetCanInstall()) draw_text(font, 480, 490, tr("sticker.add_set_hint"), accent, 0.6f);
                draw_text(font, 30, 490, tr("sticker.page", {std::to_string(first/18+1), std::to_string(std::max(1, ((int)items.size()+17)/18))}), muted, 0.6f);
            }
        }
        if (!settings_open && gifs_open) {
            tool_backdrop(36, 508);
            draw_text(font, 30, 67, tr("gif.title"), ink, 0.8f);
            draw_text(font, 500, 67, fit_text(font, tr("sticker.page", {std::to_string(gif_index / 12 + 1),
                std::to_string(std::max(1, ((int)tg.gifs().items.size() + 11) / 12))}), 430, 0.5f), muted, 0.5f);
            skin_row(30, 73, 900, 38, false, white);
            ui_skin::icon(ui_skin::Icon::Search, 42, 81, 22);
            draw_text(font, 76, 100, fit_text(font, tg.gifs().query.empty() ? tr("gif.search_hint") : tg.gifs().query, 835, 0.65f), ink, 0.65f);
            draw_text(font, 30, 126, fit_text(font, tg.gifStatus(), 900, 0.5f), muted, 0.5f);
            const auto& items = tg.gifs().items;
            if (gif_next_page >= 0 && !tg.gifs().pending) {
                if (gif_next_page < (int)items.size()) gif_index = gif_next_page;
                gif_next_page = -1;
            }
            gif_index = std::clamp(gif_index, 0, std::max(0, (int)items.size() - 1));
            const int first = gif_index / 12 * 12;
            for (int i = first; i < std::min(first + 12, (int)items.size()); ++i) {
                const int x = 30 + (i - first) % 4 * 225, y = 130 + (i - first) / 4 * 106;
                skin_row(x, y, 219, 100, i == gif_index, i == gif_index ? selected : white);
                if (!decode_cooldown) tg.ensureGif(i);
                auto* texture = textures.getGif(items[i].path, items[i], tg.gifs().generation, i);
                if (!texture) texture = textures.getGif(items[i].fallback, items[i], tg.gifs().generation, i, true);
                if (texture) {
                    const float w = vita2d_texture_get_width(texture), h = vita2d_texture_get_height(texture);
                    const float scale = std::min(203.f / w, 84.f / h);
                    draw_texture_fit(texture, x + (219 - w * scale) / 2, y + (100 - h * scale) / 2, 203, 84);
                } else {
                    ui_skin::icon(ui_skin::Icon::Play, x + 94, y + 12, 28);
                    draw_text(font, x + 12, y + 76, fit_text(font, items[i].title.empty() ? "GIF" : items[i].title, 195, 0.5f), muted, 0.5f);
                }
            }
            skin_row(30, 456, 290, 38, false, white);
            skin_row(335, 456, 290, 38, false, white);
            skin_row(640, 456, 290, 38, false, white);
            draw_text(font, 42, 482, tr("gif.previous"), first == 0 ? muted : ink, 0.55f);
            draw_text(font, 347, 482, tr("gif.saved"), ink, 0.55f);
            draw_text(font, 652, 482, tr("gif.next"), tg.gifs().end && first + 12 >= (int)items.size() ? muted : ink, 0.55f);
            draw_text(font, 30, 528, fit_text(font, tr("gif.footer"), 860, 0.5f), ink, 0.5f);
        }
        if (preview_sticker || preview_gif) {
            const float max_width = preview_gif ? 600.f : 360.f;
            vita2d_draw_rectangle(0, 36, 960, 472, RGBA8(0, 0, 0, 150));
            skin_frame(480 - max_width / 2 - 22, 72, max_width + 44, 400, separator, white);
            auto picture = preview_sticker && !preview_sticker->animated ? textures.get(preview_sticker->full_path) : animation.texture();
            if (!picture) picture = textures.get(preview_sticker ? preview_sticker->path : preview_gif->path);
            if (!picture && preview_gif) picture = textures.get(preview_gif->fallback);
            if (picture) {
                const float w = vita2d_texture_get_width(picture), h = vita2d_texture_get_height(picture);
                const float scale = std::min(max_width / w, 360.f / h);
                draw_texture_fit(picture, 480 - w * scale / 2, 272 - h * scale / 2, max_width, 360);
            } else draw_text(font, 330, 280, fit_text(font, tr(preview_gif ? "gif.loading" : "sticker.loading"), 300, 0.65f), muted, 0.65f);
            if (preview_sticker && preview_sticker->set_id) {
                rounded_rect(280, 430, 400, 42, white);
                draw_text(font, 300, 458, fit_text(font, tr("sticker.preview_set"), 360, 0.55f), accent, 0.55f);
            }
        }
        draw_overlays.finish();
        performance::SlowScope draw_tools(performance::SlowPhase::Ui,"draw_tools");
        if (recorder.active()) {
            const auto state = recorder.state();
            tool_backdrop(0, 544);
            draw_text(font, 28, 34, tr("upload.voice_message"), ink, 0.85f);
            skin_frame(160, 110, 640, 280, separator, white);
            const auto& levels = recorder.levels();
            for (size_t i = 0; i < levels.size(); ++i) {
                const float height = 4.f + levels[i] * 1.8f;
                const float x = 197.f + i * 9.f, y = 250.f - height / 2;
                const float radius = std::min(3.f, height / 2);
                const unsigned int color = state == AudioRecordingState::Error ? muted : accent;
                if (height > 2 * radius) vita2d_draw_rectangle(x - radius, y + radius, 2 * radius, height - 2 * radius, color);
                vita2d_draw_fill_circle(x, y + radius, radius, color);
                vita2d_draw_fill_circle(x, y + height - radius, radius, color);
            }
            const std::string timer = clock_text((int64_t)recorder.seconds() * 1000000);
            draw_text(font, 480 - rich_text_width(font, 0.8f, timer.c_str()) / 2, 422, timer,
                state == AudioRecordingState::Recording ? RGBA8(210,45,45,255) : ink, 0.8f);
            if (state == AudioRecordingState::Encoding) skin_bar(240, 365, 480, 8, recorder.conversionPercent() / 100.f, separator, accent);
            if (state == AudioRecordingState::Review && recorder.seconds() > 0)
                skin_bar(240, 365, 480, 6, (float)recorder.previewSeconds() / recorder.seconds(), separator, accent);
            const char* action = state == AudioRecordingState::Recording ? "vnr.action.stop" :
                state == AudioRecordingState::Review ? "vnr.action.send" : state == AudioRecordingState::Error ? "vnr.action.retry" : "vnr.action.record";
            skin_row(50, 444, 260, 42, state != AudioRecordingState::Encoding, selected);
            skin_row(350, 444, 260, 42, false, white);
            skin_row(650, 444, 260, 42, false, white);
            draw_text(font, 66, 472, fit_text(font, tr(action), 228, 0.6f), state == AudioRecordingState::Encoding ? muted : ink, 0.6f);
            draw_text(font, 366, 472, fit_text(font, tr("menu.rec_preview"), 228, 0.6f), state == AudioRecordingState::Review ? ink : muted, 0.6f);
            draw_text(font, 666, 472, fit_text(font, tr("vnr.cancel"), 228, 0.6f), ink, 0.6f);
            std::string status = audio_error.empty() ? recorder.status() : audio_error;
            if (state == AudioRecordingState::Review && audio_error.empty() && !recorder.previewError().empty()) status = recorder.previewError();
            draw_text(font, 28, 528, fit_text(font, status, 850, 0.55f), ink, 0.55f);
        } else if (circle_recorder.active()) {
            using RecordingState = VideoNoteRecorder::State;
            auto state = circle_recorder.state();
            tool_backdrop(0, 544);
            draw_text(font, 28, 34, tr("vnr.title"), ink, 0.85f);
            draw_text(font, 590, 34, circle_recorder.rearCamera() ? tr("vnr.rear_camera") : tr("vnr.front_camera"), muted, 0.6f);
            auto* preview = circle_playback && circle_playback->texture() ? circle_playback->texture() : circle_recorder.preview();
            if (preview) draw_texture_fit(preview, 290, 56, 380, 380);
            std::string timer = tr("vnr.timer", {std::to_string(circle_recorder.seconds())});
            if (state == RecordingState::Encoding) timer = tr("vnr.encoding_percent", {std::to_string(circle_recorder.percent())});
            draw_text(font, 350, 470, timer, state == RecordingState::Recording ? RGBA8(210,45,45,255) : ink, 0.7f);
            if (state == RecordingState::Recording) {
                skin_bar(320, 490, 320, 6, circle_recorder.level() / 100.f, separator, accent);
            }
            draw_text(font, 28, 528, fit_text(font, circle_error.empty() ? circle_recorder.status() : circle_error, 700, 0.55f), ink, 0.55f);
        }
        if ((chat_ui_visible() || tool_menu_visible()) && !tg.historyAnchorPending()) {
            float t = chat_menu_open ? std::min(1.f, (float)(now - chat_menu_since) / 140000.f) : 0.f;
            const float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
            if (chat_menu_open) {
                const auto& items = chat_menu_snapshot;
                const int first = menu_first((int)items.size(), chat_menu_sel);
                const int visible = std::min(history_menu_rows(), (int)items.size());
                const float h = (float)visible * kMenuRowHeight + 2 * kMenuPad;
                const float dx = (1.f - e) * 36.f, dy = (1.f - e) * 30.f, y0 = kMenuBottom - h + dy;
                const float rows_top = y0 + kMenuPad;
                // The menu PNG has a 4 px transparent margin around its body; fade it with the same easing.
                const unsigned int fade = RGBA8(255, 255, 255, (unsigned int)(255.f * e));
                const bool menu_skin = ui_skin::nine(ui_skin::Slice::Menu, kMenuX + dx - 4, y0 - 4, kMenuWidth + 8, h + 8, fade);
                if (!menu_skin) {
                    glass_panel(kMenuX + dx, y0, kMenuWidth, h, scale_alpha(RGBA8(52, 58, 68, 232), e), scale_alpha(RGBA8(20, 25, 33, 238), e));
                    vita2d_draw_rectangle(kMenuX + 10 + dx, y0, kMenuWidth - 20, 1, scale_alpha(RGBA8(255, 255, 255, 90), e));
                }
                chat_menu_sel = std::max(0, std::min(chat_menu_sel, (int)items.size() - 1));
                for (int i = first; i < first + visible; ++i) {
                    if (i == chat_menu_sel && !(menu_skin && ui_skin::nine(ui_skin::Slice::MenuItemFocus, kMenuX + kMenuPad + dx,
                            rows_top + (i - first) * kMenuRowHeight + 2, kMenuWidth - 2 * kMenuPad, kMenuFocusHeight, fade)))
                        glass_panel(kMenuX + kMenuPad + dx, rows_top + (i - first) * kMenuRowHeight + 2, kMenuWidth - 2 * kMenuPad, kMenuFocusHeight,
                            scale_alpha(RGBA8(255, 255, 255, 62), e), scale_alpha(RGBA8(255, 255, 255, 26), e), 7);
                    if (i && !menu_skin) vita2d_draw_rectangle(kMenuX + 8 + dx, rows_top + (i - first) * kMenuRowHeight - kMenuPad, kMenuWidth - 16, 1, scale_alpha(RGBA8(255, 255, 255, 36), e));
                    const std::string label = items[i].text.empty() ? tr(items[i].label) : items[i].text;
                    bool with_icon = items[i].icon >= 0 &&
                        ui_skin::icon((ui_skin::Icon)items[i].icon, kMenuX + 16 + dx, rows_top + (i - first) * kMenuRowHeight + 7, 22, fade);
                    if (!with_icon && items[i].icon == (int)ui_skin::Icon::Check) {
                        const float x = kMenuX + 19 + dx, y = rows_top + (i - first) * kMenuRowHeight + 15;
                        vita2d_draw_line(x, y, x + 5, y + 5, fade);
                        vita2d_draw_line(x + 5, y + 5, x + 15, y - 6, fade);
                        with_icon = true;
                    }
                    const float label_shift = with_icon ? 30.f : 0.f;
                    draw_text(font, kMenuX + 20 + dx + label_shift, rows_top + (i - first) * kMenuRowHeight + 26, fit_text(font, label, kMenuWidth - 40 - (int)label_shift, 0.62f),
                              scale_alpha(menu_skin ? ink : RGBA8(240, 244, 249, 255), e), 0.62f);
                }
                if ((int)items.size() > visible) {
                    const float track = h - 12.f, thumb = track * visible / items.size();
                    const float thumb_y = y0 + 6 + (track - thumb) * first / ((int)items.size() - visible);
                    if (!ui_skin::nine(ui_skin::Slice::ScrollThumb, kMenuX + kMenuWidth - 7 + dx, thumb_y, 4, thumb, fade))
                        vita2d_draw_rectangle(kMenuX + kMenuWidth - 7 + dx, thumb_y, 3, thumb, scale_alpha(muted, e));
                }
            }
            if (media_ui_alpha > 0.01f) draw_menu_button(dark, chat_menu_open ? e : 0.f, media_ui_alpha);
        }
        else if (kb.active() && !settings.combined_view && !settings_open && !qr_scanner.active() && (tg.currentChatId() != 0 || tg.topicsOpen() || tg.canBrowse()) &&
                 upload_picker.stage == UploadPicker::Stage::Closed)
            draw_menu_button(dark);   // stays visible under the system keyboard, like the attach button
        {
            static UploadPicker::Stage previous_stage = UploadPicker::Stage::Closed;
            if (upload_picker.stage == UploadPicker::Stage::Menu && previous_stage != UploadPicker::Stage::Menu) attach_menu_since = now;
            previous_stage = upload_picker.stage;
        }
        if (!settings.combined_view && chat_ui_visible() && tg.currentChatId() && post_open && !photo_open && !circle_recorder.active() && !recorder.active() &&
            (!video_fullscreen || media_ui_alpha > 0.01f))
            draw_attach_button(dark, 0, video_fullscreen ? media_ui_alpha : 1.f);   // fades with the fullscreen controls
        if (chat_ui_visible() && tg.ready() && post_open && !photo_open && !video_fullscreen &&
            !stickers_open && !gifs_open && !reactions_open && !upload_picker.active() && !chat_menu_open && !chat_menu_waiting &&
            !circle_recorder.active() && !recorder.active() && message_index >= 0 && message_index < (int)tg.messages().size() &&
            tg.canDeleteMessage(tg.messages()[message_index].id)) {
            const int delete_top = history_view.combined ? history_view.top : 68;
            skin_box(ui_skin::Slice::DateChip, 766, delete_top, 168, 28, selected, true);
            ui_skin::icon(ui_skin::Icon::Trash, 773, delete_top+4, 20);
            draw_text(font, 799, delete_top+19, fit_text(font, tr("menu.delete_message"), 128, 0.46f), ink, 0.46f);
            delete_hit = {true, 766, (float)delete_top, 168, 28};
            delete_hit_message = tg.messages()[message_index].id;
        }
        if (upload_picker.active()) {
            using Stage = UploadPicker::Stage;
            const char* kind_key = upload_picker.kind == UploadPicker::Kind::File ? "upload.file" : "upload.media";
            if (upload_picker.stage == Stage::Menu) {
                // Compact glass popup growing out of the attach button (~140 ms ease-out).
                float t = std::min(1.f, (float)(now - attach_menu_since) / 140000.f);
                const float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
                const float dy = history_view.combined ? 0.f : (1.f - e) * 30.f, dx = history_view.combined ? 0.f : -(1.f - e) * 36.f; // grows out of the circle
                const unsigned int fade = RGBA8(255, 255, 255, (unsigned int)(255.f * e));
                const auto items = attach_menu_items();
                const int panel_height = (int)items.size() * attach_menu_row + 2 * kMenuPad;
                const int left = history_view.attachLeft(), top = history_view.attachTop((int)items.size());
                const int panel_top = top - kMenuPad;
                upload_picker.selected = std::max(0, std::min(upload_picker.selected, (int)items.size()-1));
                const bool menu_skin = ui_skin::nine(ui_skin::Slice::Menu, left - 4 + dx, panel_top - 4 + dy, ui_layout::attach_width + 8, panel_height + 8, fade);
                if (!menu_skin) {
                    glass_panel(left + dx, panel_top + dy, 300, panel_height, scale_alpha(RGBA8(52, 58, 68, 232), e), scale_alpha(RGBA8(20, 25, 33, 238), e));
                    vita2d_draw_rectangle(left + 10 + dx, panel_top + dy, 280, 1, scale_alpha(RGBA8(255, 255, 255, 90), e));
                }
                for (int i = 0; i < (int)items.size(); ++i) {
                    if (i == upload_picker.selected && !(menu_skin && ui_skin::nine(ui_skin::Slice::MenuItemFocus, left + 8 + dx,
                        top + 2 + i * attach_menu_row + dy, ui_layout::attach_width - 16, ui_layout::menu_focus_height, fade)))
                        glass_panel(left + 8 + dx, top + 2 + i * 40 + dy, 284, 36, scale_alpha(RGBA8(255, 255, 255, 62), e), scale_alpha(RGBA8(255, 255, 255, 26), e), 7);
                    if (i && !menu_skin) vita2d_draw_rectangle(left + 8 + dx, top + i * 40 + dy, 284, 1, scale_alpha(RGBA8(255, 255, 255, 36), e));
                    const bool icon = items[i].icon >= 0 && ui_skin::icon((ui_skin::Icon)items[i].icon,
                        left + 14 + dx, top + 10 + i * 40 + dy, 22, fade);
                    draw_text(font, left + 20 + dx + (icon ? 28 : 0), top + 30 + i * 40 + dy,
                        fit_text(font, tr(items[i].label), icon ? 246 : 274, 0.62f),
                        scale_alpha(menu_skin ? ink : RGBA8(240, 244, 249, 255), e), 0.62f);
                }
                if (!settings.combined_view) draw_attach_button(dark, e);
            } else if (upload_picker.stage == Stage::Browse) {
                tool_backdrop(36, 508);
                const bool preview = upload_picker.kind == UploadPicker::Kind::Media;
                const float list_w = preview ? 496.f : 896.f;   // same split as the review form
                draw_text(font, 30, 68, fit_text(font, upload_picker.path.empty() ? tr("picker.drives") : upload_picker.path == UploadPicker::LatestView ? tr("upload.latest") : upload_picker.path, 900, 0.64f), ink, 0.64f);
                const std::string sort_hint = tr("upload.sort_hint", {tr(upload_picker.sortKey())});
                const int sort_width = rich_text_width(font, 0.48f, sort_hint.c_str());
                const int sort_x = 932 - sort_width;
                draw_text(font, 32, 97, fit_text(font, tr(kind_key) + "  |  " +
                    tr("upload.review_title", {std::to_string(upload_picker.items.size()), std::to_string(UploadPicker::MaxItems)}) + "  |  " +
                    tr(upload_picker.show_hidden ? "upload.hidden_on" : "upload.hidden_off"), std::max(0, sort_x - 48), 0.48f),
                    upload_picker.items.size() == UploadPicker::MaxItems ? accent : muted, 0.48f);
                draw_text(font, sort_x, 97, sort_hint, accent, 0.48f);
                skin_frame(24, 109, list_w + 16, 356, separator, white);
                const int first = (upload_picker.selected / 8) * 8;
                for (int row = 0; row < 8 && first + row < (int)upload_picker.entries.size(); ++row) {
                    const auto& entry = upload_picker.entries[first + row];
                    const bool marked = upload_picker.isMarked(entry);
                    if (marked) vita2d_draw_rectangle(32, 117 + row * 43, list_w, 40, scale_alpha(accent, 0.20f));
                    if (first + row == upload_picker.selected) skin_row(32, 117 + row * 43, list_w, 40, true, selected, true);
                    // Draw the marked-file outline over the cursor skin so it remains distinct.
                    if (marked) {
                        const float row_y = 117 + row * 43;
                        constexpr float border = 4.f;
                        vita2d_draw_rectangle(32, row_y, list_w, border, accent);
                        vita2d_draw_rectangle(32, row_y + 40 - border, list_w, border, accent);
                        vita2d_draw_rectangle(32, row_y + border, border, 40 - 2 * border, accent);
                        vita2d_draw_rectangle(32 + list_w - border, row_y + border, border, 40 - 2 * border, accent);
                    }
                    const std::string size = entry.dir ? "" : UploadPicker::formatSize(entry.size);
                    const int size_width = rich_text_width(font, 0.45f, size.c_str());
                    const int date_width = rich_text_width(font, 0.45f, entry.date.c_str());
                    const int size_x = (int)(32 + list_w - 12) - size_width;
                    const int date_x = size_x - (size.empty() ? 0 : 12) - date_width;
                    const int name_right = !entry.date.empty() ? date_x - 12 : !size.empty() ? size_x - 12 : (int)(32 + list_w - 12);
                    draw_text(font, 42, 144 + row * 43, fit_text(font, !entry.label.empty() ? entry.label : entry.dir ? entry.name + "/" : entry.name, std::max(0, name_right - 42), 0.65f), ink, 0.65f);
                    if (!entry.date.empty()) draw_text(font, date_x, 143 + row * 43, entry.date, muted, 0.45f);
                    if (!size.empty()) draw_text(font, size_x, 143 + row * 43, size, muted, 0.45f);
                }
                if (preview) {
                    skin_frame(52 + list_w, 109, 884 - list_w, 356, separator, white);
                    if (upload_picker.selected >= 0 && upload_picker.selected < (int)upload_picker.entries.size() &&
                        !upload_picker.entries[upload_picker.selected].dir) {
                        const std::string full = upload_picker.fullPath(upload_picker.entries[upload_picker.selected]);
                        if (UploadPicker::matches(UploadPicker::Kind::Photo, full))
                            draw_texture_fit(textures.get(full), 60 + list_w, 117, 868 - list_w, 340);
                        else draw_text(font, 60 + list_w, 150, tr("upload.media"), muted, 0.65f);
                    }
                }
                if (upload_picker.entries.empty() && upload_picker.error.empty())
                    draw_text(font, 42, 144, fit_text(font, upload_picker.info.empty() ? tr("upload.empty") : upload_picker.info, 520, 0.5f), muted, 0.5f);
                if (!upload_picker.error.empty()) draw_text(font, 30, 484, upload_picker.error, ink, 0.55f);
                draw_text(font, 28, 528, tr("upload.browse_footer"), ink, 0.6f);
            } else {
                tool_backdrop(36, 508);
                const int count = (int)upload_picker.items.size();
                draw_text(font, 30, 76, tr("upload.review_title", {std::to_string(count), std::to_string(UploadPicker::MaxItems)}), ink, 0.8f);
                skin_frame(24, 109, 512, 356, separator, white);
                const int first = (upload_picker.selected / 8) * 8;
                const char* add_key[] = {"upload.add_media", "upload.add_file"};
                for (int row = 0; row < 8 && first + row < upload_picker.reviewRows(); ++row) {
                    const int index = first + row;
                    if (index == upload_picker.selected) skin_row(32, 117 + row * 43, 496, 40, true, selected, true);
                    std::string label;
                    const bool item_row = index > 0 && index <= count;
                    if (index == 0) label = tr("upload.send", {std::to_string(count)});
                    else if (item_row) label = upload_picker.items[index - 1].name;
                    else if (index == upload_picker.reviewRows() - 1) label = tr(upload_picker.captionKey());
                    else label = tr(add_key[index - count - 1]);
                    draw_text(font, 42, 144 + row * 43, fit_text(font, label, item_row ? 400 : 470, 0.65f),
                              index == 0 ? (count == 0 ? muted : accent) : ink, 0.65f);
                    if (item_row) draw_text(font, 450, 143 + row * 43, UploadPicker::formatSize(upload_picker.items[index - 1].size), muted, 0.45f);
                }
                skin_frame(548, 109, 388, 356, separator, white);
                const auto* focus = upload_picker.focusedItem();
                if (upload_picker.textSelected()) {
                    int y = 143;
                    for (const auto& line : wrap_text(font, upload_picker.caption, 350)) {
                        if (y > 445) break;
                        draw_text(font, 566, y, line, ink, 0.65f); y += 25;
                    }
                } else if (focus) {
                    if (focus->kind == UploadPicker::Kind::Photo) draw_texture_fit(textures.get(focus->path), 556, 117, 372, 340);
                    else if (focus->kind == UploadPicker::Kind::Video) {
                        draw_texture_fit(animation.texture(), 556, 117, 372, 310);
                        draw_text(font, 560, 452, fit_text(font, animation.status(), 360, 0.45f), muted, 0.45f);
                    } else draw_text(font, 566, 150, fit_text(font, tr("upload.file_info", {focus->name, UploadPicker::formatSize(focus->size)}), 350, 0.55f), muted, 0.55f);
                }
                draw_text(font, 28, 528, tr(focus && focus->kind == UploadPicker::Kind::Video ? "upload.review_footer_video" : "upload.review_footer"), ink, 0.55f);
            }
        }
        if (confirm_leave) {
            if (!ui_skin::nine(ui_skin::Slice::Dialog, 131, 131, 698, 280)) {
                vita2d_draw_rectangle(135, 135, 690, 272, white);
                vita2d_draw_rectangle(135, 135, 690, 4, accent);
            }
            const char* prefix = confirm_action == 0 ? "leave" : confirm_action == 1 ? "delete" : "clear";
            draw_text(font, 165, 195, fit_text(font, tr((std::string(prefix) + ".title").c_str(), {tg.currentChatTitle()}), 630, 0.8f), ink, 0.8f);
            draw_text(font, 165, 240, fit_text(font, tr((std::string(prefix) + ".warning").c_str()), 630, 0.6f), muted, 0.6f);
            skin_row(160, 300, 640, 44, true, selected, true);
            draw_text(font, 180, 329, tr((std::string(prefix) + ".confirm").c_str()), ink, 0.7f);
            skin_row(160, 352, 640, 44, true, selected, true);
            draw_text(font, 180, 381, tr("leave.cancel"), ink, 0.7f);
        }
        if (bot_panel) {
            const unsigned surface = ui_skin::surface_color(white) | 0xff000000u;
            const unsigned label = ui_color::tone(ink, surface, surface, 4.5);
            const unsigned edge = ui_color::tone(ui_skin::color("focus", accent), surface, surface, 3.0);
            vita2d_enable_clipping(); vita2d_set_clip_rectangle(bot_geometry.left, 36, 960, 544);
            if (bot_panel == 1) {
                const int count = bot_buttons::size(bot_snapshot);
                const auto sheet = bot_buttons::command_sheet(count, bot_focus, now - bot_sheet_since, bot_sheet_closing, bot_geometry.left);
                const int top = sheet.top + sheet.offset;
                const int dim = 100 * (544 - sheet.top - sheet.offset) / (544 - sheet.top);
                vita2d_draw_rectangle(bot_geometry.left, 36, 960 - bot_geometry.left, 508, RGBA8(0, 0, 0, dim));
                glass_panel(bot_geometry.left, top, 960 - bot_geometry.left, 544 - sheet.top, surface, surface, 10);
                vita2d_draw_rectangle((bot_geometry.left + 960) / 2 - 24, top + 8, 48, 4, edge);
                draw_text(font, bot_geometry.x(32), top + 34, tr("bot.menu"), label, 0.68f);
                for (const auto& cell : sheet.cells()) {
                    const auto& button = bot_snapshot.rows[cell.row][0];
                    const unsigned fill = ui_skin::color("selected", selected) | 0xff000000u;
                    glass_panel(cell.x, cell.y, cell.w, cell.h, fill, fill, 7);
                    const unsigned key_ink = ui_color::tone(ink, fill, fill, 4.5);
                    if (cell.row == bot_focus) {
                        const unsigned outline = ui_color::tone(edge, fill, fill, 3.0);
                        vita2d_draw_rectangle(cell.x, cell.y, cell.w, 2, outline);
                        vita2d_draw_rectangle(cell.x, cell.y + cell.h - 2, cell.w, 2, outline);
                        vita2d_draw_rectangle(cell.x, cell.y + 2, 2, cell.h - 4, outline);
                        vita2d_draw_rectangle(cell.x + cell.w - 2, cell.y + 2, 2, cell.h - 4, outline);
                        draw_settings_text(font, bot_marquee, cell.row, true, cell.x + 12, cell.y + 4, cell.w - 24, cell.h - 8, cell.y + 35, button.label, key_ink, 0.62f, now);
                    } else draw_text(font, cell.x + 12, cell.y + 35, fit_text(font, button.label, cell.w - 24, 0.62f), key_ink, 0.62f);
                }
                draw_text(font, bot_geometry.x(48), 492 + sheet.offset, std::to_string(count ? bot_focus + 1 : 0) + "/" + std::to_string(count), label, 0.55f);
                for (int next = 0; next < 2; ++next) {
                    if ((!next && sheet.first == 0) || (next && sheet.first + sheet.count >= count)) continue;
                    glass_panel(bot_geometry.x(next ? 390 : 200), 474 + sheet.offset, bot_geometry.span(next ? 390 : 200, 165), 26, selected | 0xff000000u, selected | 0xff000000u, 5);
                    draw_text(font, bot_geometry.x(next ? 465 : 275), 494 + sheet.offset, next ? ">" : "<", ui_color::on(selected), 0.55f);
                }
                draw_text(font, bot_geometry.x(48), 530 + sheet.offset, fit_text(font, tr("bot.panel_hint"), bot_geometry.width(675), 0.55f), label, 0.55f);
                const auto back = sheet.back();
                glass_panel(back.x, back.y, back.w, back.h, selected | 0xff000000u, selected | 0xff000000u, 6);
                draw_text(font, bot_geometry.x(754), back.y + 28, fit_text(font, tr("bot.back"), bot_geometry.width(144), 0.55f), ui_color::on(selected), 0.55f);
            } else {
                vita2d_draw_rectangle(bot_geometry.left, bot_panel == 2 ? 318 : 36, 960 - bot_geometry.left, bot_panel == 2 ? 226 : 508, surface);
                draw_text(font, bot_geometry.x(48), bot_panel == 2 ? 347 : 78, tr(bot_panel == 4 ? "bot.response" : bot_panel == 1 ? "bot.menu" : bot_panel == 2 ? "bot.keyboard" : "bot.buttons") + (bot_panel == 4 ? "" : "   L/R"), label, 0.8f);
                const auto response_lines = bot_panel == 4 ? wrap_text(font, bot_alert, bot_geometry.width(864)) : std::vector<std::string>{};
                const auto page = bot_buttons::page(bot_snapshot, bot_focus, bot_panel == 2 ? 2 : 6);
                const int count = bot_panel == 4 ? (int)response_lines.size() : bot_buttons::size(bot_snapshot);
                const int first = bot_panel == 4 ? bot_focus / 6 * 6 : page.first;
                if (bot_panel == 4) {
                    int y = 147;
                    for (int line = first; line < count && y < 465; ++line, y += scaled_px(26))
                        draw_text(font, bot_geometry.x(48), y, response_lines[line], label, 0.68f);
                }
                const auto cells = bot_buttons::cells(bot_snapshot, bot_panel == 2 ? 358 : 142, 44, first, page.count, bot_geometry.x(48), bot_geometry.x(912) - bot_geometry.x(48));
                int index = first;
                for (const auto& cell : cells) {
                    const auto& button = bot_snapshot.rows[cell.row][cell.column];
                    const unsigned fill = ui_skin::color("selected", selected) | 0xff000000u;
                    glass_panel(cell.x, cell.y, cell.w, cell.h, fill, fill, 7);
                    const unsigned key_ink = ui_color::tone(ink, fill, fill, 4.5);
                    if (bot_panel == 2 && index == bot_focus)
                        draw_settings_text(font, bot_marquee, index, true, cell.x + 12, cell.y + 2, cell.w - 24, cell.h - 4, cell.y + 29, button.label, key_ink, 0.62f, now);
                    else draw_text(font, cell.x + 12, cell.y + 29, fit_text(font, button.label, cell.w - 24, 0.62f), key_ink, 0.62f);
                    if (index == bot_focus) {
                        const unsigned outline = ui_color::tone(edge, fill, fill, 3.0);
                        vita2d_draw_rectangle(cell.x, cell.y, cell.w, 2, outline);
                        vita2d_draw_rectangle(cell.x, cell.y + cell.h - 2, cell.w, 2, outline);
                        vita2d_draw_rectangle(cell.x, cell.y + 2, 2, cell.h - 4, outline);
                        vita2d_draw_rectangle(cell.x + cell.w - 2, cell.y + 2, 2, cell.h - 4, outline);
                        if (bot_panel != 2) draw_settings_text(font, bot_marquee, index, true, bot_geometry.x(48), 91, bot_geometry.width(864), 40, 119, button.label, label, 0.62f, now);
                    }
                    ++index;
                }
                if (!count) draw_text(font, bot_geometry.x(48), 182, tr("bot.empty"), label, 0.68f);
                draw_text(font, bot_geometry.x(48), 492, std::to_string(count ? bot_focus + 1 : 0) + "/" + std::to_string(count), label, 0.55f);
                if (first > 0) {
                    glass_panel(bot_geometry.x(200), 474, bot_geometry.span(200, 165), 26, selected | 0xff000000u, selected | 0xff000000u, 5);
                    draw_text(font, bot_geometry.x(275), 494, "<", ui_color::on(selected), 0.55f);
                }
                if (first + (bot_panel == 4 ? 6 : page.count) < count) {
                    glass_panel(bot_geometry.x(390), 474, bot_geometry.span(390, 165), 26, selected | 0xff000000u, selected | 0xff000000u, 5);
                    draw_text(font, bot_geometry.x(465), 494, ">", ui_color::on(selected), 0.55f);
                }
                draw_text(font, bot_geometry.x(575), 492, fit_text(font, tg.botStatus(), bot_geometry.width(337), 0.55f), label, 0.55f);
                draw_text(font, bot_geometry.x(48), 530, fit_text(font, tr("bot.panel_hint"), bot_geometry.width(675), 0.55f), label, 0.55f);
                glass_panel(bot_geometry.x(740), 502, bot_geometry.span(740, 172), 40, selected | 0xff000000u, selected | 0xff000000u, 6);
                draw_text(font, bot_geometry.x(754), 530, fit_text(font, tr("bot.back"), bot_geometry.width(144), 0.55f), ui_color::on(selected), 0.55f);
            }
            vita2d_disable_clipping();
        }
        if (!settings_open && tool_window_open() && !kb.active()) {
            skin_box(ui_skin::Slice::Dialog, chat_view::ToolWindow::back_left, chat_view::ToolWindow::back_top,
                chat_view::ToolWindow::back_width, chat_view::ToolWindow::back_height, white, true);
            draw_text(font, chat_view::ToolWindow::back_left+8, chat_view::ToolWindow::back_top+19,
                fit_text(font, tr("hint.back"), chat_view::ToolWindow::back_width-16, 0.48f), ink, 0.48f);
        }
        radial::draw(font, compose_wheel, compose_geometry, compose_labels, now, white, ink, selected, accent, separator);
        draw_tools.finish();
        if (kb.active()) {
            performance::SlowScope keyboard_draw(performance::SlowPhase::Ui,"keyboard_draw");
            kb.draw(font);
        }
        performance::SlowScope draw_dialogs(performance::SlowPhase::Ui,"draw_dialogs");
#if VITAGRAM_WEBKIT
        if (tg.botWebApp().phase != TelegramClient::BotWebApp::Closed) {
            const bool fullscreen_webapp = webapp_view.active() && webapp_view.fullscreen();
            const int webapp_top = fullscreen_webapp ? 0 : 100, webapp_height = fullscreen_webapp ? 544 : 392;
            const unsigned surface = ui_skin::surface_color(white) | 0xff000000u;
            const unsigned text = ui_color::tone(ink, surface, surface, 4.5);
            vita2d_draw_rectangle(0, fullscreen_webapp ? 0 : 36, 960, fullscreen_webapp ? 544 : 508, surface);
            const unsigned header = webapp_header_fill ? webapp_header_fill : surface;
            if (!fullscreen_webapp) {
                vita2d_draw_rectangle(0, 36, 960, 64, header);
                if (webapp_bottom_fill) vita2d_draw_rectangle(0, 492, 960, 52, webapp_bottom_fill);
                draw_text(font, 32, 65, fit_text(font, tg.botWebApp().label, 880, 0.7f), ui_color::tone(ink, header, header, 4.5), 0.7f);
            }
            if (webapp_background_fill) vita2d_draw_rectangle(0, webapp_top, 960, webapp_height, webapp_background_fill);
            if (tg.botWebApp().phase == TelegramClient::BotWebApp::Confirm) {
                int y = 135;
                const auto download = webkit_download.snapshot();
                const bool installed = download.state == WebKitDownload::State::Ready;
                const auto message = installed ? tr("webapp.confirm") : tr("webapp.engine_needed");
                for (const auto& line : wrap_text(font, message, 864)) { draw_text(font, 48, y, line, text, 0.7f); y += 30; }
                std::string status;
                if (download.state == WebKitDownload::State::Available) status = std::to_string((download.total + (1 << 20) - 1) >> 20) + " MiB";
                else if (download.state == WebKitDownload::State::Downloading) status = tr("webapp.engine_downloading") + " " + std::to_string(download.bytes >> 20) + " / " + std::to_string((download.total + (1 << 20) - 1) >> 20) + " MiB";
                else if (download.state == WebKitDownload::State::Checking) status = tr("webapp.engine_checking");
                else if (download.state == WebKitDownload::State::Failed) status = download.error;
                else if (installed) status = webapp_origin(tg.botWebApp().url);
                for (const auto& line : wrap_text(font, status, 864)) { if (y > 435) break; draw_text(font, 48, y + 36, line, text, 0.65f); y += 28; }
                if (download.busy()) draw_buffering_spinner(936, 65, now, accent, 12);
                glass_panel(webapp_primary_rect.x, webapp_primary_rect.y, webapp_primary_rect.w, webapp_primary_rect.h, selected | 0xff000000u, selected | 0xff000000u, 6);
                if (!download.busy()) draw_text(font, 345, 529, fit_text(font, tr(installed ? "webapp.open" : download.state == WebKitDownload::State::Failed ? "webapp.engine_retry" : "webapp.engine_download"), 320, 0.6f), ui_color::on(selected), 0.6f);
            } else if (webapp_view.active() && webapp_view.texture()) {
                vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, webapp_top, 960, webapp_top + webapp_height);
                auto* web_pixels = webapp_view.texture();
                vita2d_draw_texture_scale_rotate_hotspot(web_pixels, 480, webapp_top + webapp_height * 0.5f, 1, 1,
                    webapp_view.rotation() * 1.57079632679f,
                    vita2d_texture_get_width(web_pixels) * 0.5f, vita2d_texture_get_height(web_pixels) * 0.5f);
                if (webapp_pointer_visible) {
                    vita2d_draw_rectangle(webapp_x - 6, webapp_y + webapp_top, 13, 2, accent);
                    vita2d_draw_rectangle(webapp_x, webapp_y + webapp_top - 6, 2, 13, accent);
                }
                vita2d_disable_clipping();
                if (webapp_view.loading()) {
                    draw_buffering_spinner(936, fullscreen_webapp ? 24 : 65, now, accent, 12);
                }
                if (!fullscreen_webapp && webapp_main) {
                    glass_panel(webapp_primary_rect.x, webapp_primary_rect.y, webapp_primary_rect.w, webapp_primary_rect.h, selected | 0xff000000u, selected | 0xff000000u, 6);
                    draw_text(font, 345, 528, fit_text(font, webapp_main_text, 320, 0.6f), ui_color::on(selected), 0.6f);
                }
                if (!fullscreen_webapp && webapp_back) {
                    glass_panel(webapp_back_rect.x, webapp_back_rect.y, webapp_back_rect.w, webapp_back_rect.h, selected | 0xff000000u, selected | 0xff000000u, 6);
                    draw_text(font, 46, 529, fit_text(font, tr("bot.back"), 240, 0.55f), ui_color::on(selected), 0.55f);
                }
            } else if (tg.botWebApp().phase == TelegramClient::BotWebApp::Loading)
                draw_buffering_spinner(936, 65, now, accent, 12);
            const auto error = !webapp_error.empty() ? webapp_error : !webapp_view.error().empty() ? webapp_view.error() : tg.botWebApp().error;
            if (!error.empty()) {
                const int error_y = webapp_top + webapp_height - 72;
                vita2d_draw_rectangle(0, error_y, 960, 72, surface);
                draw_text(font, 32, error_y + 32, fit_text(font, error, 896, 0.6f), text, 0.6f);
            }
            if (!fullscreen_webapp) {
                glass_panel(webapp_close_rect.x, webapp_close_rect.y, webapp_close_rect.w, webapp_close_rect.h, selected | 0xff000000u, selected | 0xff000000u, 6);
                draw_text(font, 754, 529, fit_text(font, tr("webapp.close"), 144, 0.55f), ui_color::on(selected), 0.55f);
            }
            if (webapp_keyboard.active()) {
                performance::SlowScope keyboard_draw(performance::SlowPhase::Ui,"keyboard_draw");
                webapp_keyboard.draw(font);
            }
        }
#endif
        if (!pending_web_link.empty()) {
            vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0,0,0,140));
            rounded_rect(100, 62, 760, 428, white);
            draw_text(font, 130, 101, tr("link.confirm_title"), ink, 0.8f);
            auto address_lines = wrap_text(font, pending_web_link, 700);
            for (int i = 0; i < 12 && web_link_scroll+i < (int)address_lines.size(); ++i)
                draw_text(font, 130, 137+i*23, address_lines[web_link_scroll+i], ink, 0.68f);
            {
                std::string scheme = pending_web_link.substr(0, 7);
                std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                if (scheme == "http://") draw_text(font, 130, 421, tr("link.insecure"), accent, 0.62f); // cleartext: readable and changeable on the network
            }
            rounded_rect(130, 437, 330, 40, !web_link_proceed ? selected : white);
            rounded_rect(500, 437, 330, 40, web_link_proceed ? selected : white);
            draw_text(font, 150, 465, tr("link.cancel"), ink, 0.7f);
            draw_text(font, 520, 465, tr("link.proceed"), accent, 0.7f);
        }
        if (spoiler_prompt) {
            vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 140));
            if (!ui_skin::nine(ui_skin::Slice::Dialog, 91, 141, 778, 263)) {
                vita2d_draw_rectangle(95, 145, 770, 255, white);
                vita2d_draw_rectangle(95, 145, 770, 4, accent);
            }
            draw_text(font, 125, 195, tr("spoiler.title"), ink, 0.8f);
            int warning_y = 242;
            for (const auto& line : wrap_text(font, tr("spoiler.warning"), 700)) { draw_text(font, 125, warning_y, line, ink, 0.62f); warning_y += 28; }
            draw_text(font, 125, 368, tr("spoiler.footer"), muted, 0.6f);
        }
        if (unsupported_video_prompt) {
            if (!ui_skin::nine(ui_skin::Slice::Dialog, 91, 141, 778, 263)) {
                vita2d_draw_rectangle(95, 145, 770, 255, white);
                vita2d_draw_rectangle(95, 145, 770, 4, accent);
            }
            draw_text(font, 125, 195, tr("unsupported.title"), ink, 0.8f);
            draw_text(font, 125, 242, tr("unsupported.reason"), ink, 0.6f);
            draw_text(font, 125, 286, tr("unsupported.question"), ink, 0.6f);
            draw_text(font, 125, 318, tr("unsupported.warning"), ink, 0.6f);
            draw_text(font, 125, 368, tr("unsupported.footer"), muted, 0.6f);
        }

        if (seek_open) {
            vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0,0,0,150));
            skin_box(ui_skin::Slice::Dialog, 180, 92, 600, 360, white, true);
            draw_text(font, 206, 130, fit_text(font, tr("menu.seek_time"), 548, 0.8f), ink, 0.8f);
            const auto& message = tg.messages()[message_index];
            draw_text(font, 206, 158, clock_text(seek_time.targetUs()) + " / " +
                clock_text((int64_t)(seek_audio ? message.voice_duration : message.video_duration) * 1000000), muted, 0.6f);
            const int64_t values[] = {seek_time.seconds / 3600, seek_time.seconds / 60 % 60, seek_time.seconds % 60};
            for (int field = 0; field < 3; ++field) {
                const int x = 260 + field * 150;
                skin_row(x, 180, 128, 140, seek_time.field == field, seek_time.field == field ? selected : separator, true);
                draw_text(font, x + 53, 212, "+", ink, 0.8f);
                char number[24]; std::snprintf(number, sizeof(number), "%02lld", (long long)values[field]);
                const int width = rich_text_width(font, 1.2f, number);
                draw_text(font, x + (128 - width) / 2, 266, number, ink, 1.2f);
                draw_text(font, x + 53, 308, "-", ink, 0.8f);
                if (field) draw_text(font, x - 16, 266, ":", muted, 1.f);
            }
            draw_text(font, 206, 354, fit_text(font, tr("media.seek_hint"), 548, 0.52f), muted, 0.52f);
            skin_row(220, 382, 230, 44, false, separator, true);
            skin_row(510, 382, 230, 44, false, separator, true);
            draw_text(font, 236, 411, fit_text(font, tr("media.seek_apply"), 198, 0.65f), ink, 0.65f);
            draw_text(font, 526, 411, fit_text(font, tr("hint.back"), 198, 0.65f), ink, 0.65f);
        }
        if (!settings_open && tg.historyAnchorPending()) {
            if (!history_view.combined) skin_backdrop(36, 508, white);
            vita2d_enable_clipping();
            vita2d_set_clip_rectangle(history_view.left(), history_view.top, 960, 544);
            const bool far = tg.historyAnchorFar(), failed = !tg.historyError().empty();
            const int left = history_view.combined ? history_view.x(32) : 90;
            const int width = history_view.combined ? history_view.width(896) : 780;
            const int top = history_view.combined ? history_view.top+18 : 145;
            const int padding = history_view.combined ? 20 : 30, text_width = history_view.combined ? width-40 : 710;
            skin_box(ui_skin::Slice::Dialog, left, top, width, history_view.combined ? 544-top-18 : 260, white, true);
            draw_text(font, left+padding, top+(history_view.combined ? 35 : 45), fit_text(font, tr(far ? "history.far_title" : failed ? "history.open_failed" : "history.loading"), width-40, 0.8f), ink, 0.8f);
            const std::string detail = far ? tr("history.far_message") : failed ? std::string(tg.historyError()) : tr("history.open_wait");
            int y = top+(history_view.combined ? 70 : 85);
            for (const auto& line : wrap_text(font, detail, text_width)) {
                if (y > (history_view.combined ? 470 : 330)) break;
                draw_text(font, left+padding, y, line, ink, 0.6f); y += 26;
            }
            if (!far && !failed) draw_buffering_spinner(history_view.center(), history_view.combined ? 420 : 335, now, accent);
            draw_text(font, left+padding, history_view.combined ? 505 : 380, fit_text(font, tr(far ? "history.far_continue" : failed ? "history.open_retry" : "history.open_cancel"), width-40, 0.55f), muted, 0.55f);
            vita2d_disable_clipping();
        }
        if (settings_open && theme_picker.active) {
            vita2d_draw_rectangle(0, 0, 960, 544, font_picker ? ui_skin::surface_color(white) | 0xff000000u : RGBA8(0,0,0,170));
            skin_box(ui_skin::Slice::Menu, ThemePicker::left, ThemePicker::top,
                ThemePicker::width, ThemePicker::height, white, true);
            draw_text(font, ThemePicker::left + 28, ThemePicker::top + 30,
                language_picker ? tr("settings.language") : font_picker ? tr("settings.font_family") : tr("settings.theme"), ink, 0.8f);
            if (font_picker || language_picker) draw_text(font, ThemePicker::left + ThemePicker::width - 160,
                ThemePicker::top + 29, tr("hint.back"), ink, 0.55f);
            for (int i = theme_picker.first; i < std::min(theme_picker.count, theme_picker.first + ThemePicker::visible); ++i) {
                const int y = ThemePicker::top + ThemePicker::header + (i - theme_picker.first) * ThemePicker::row_height;
                if (font_picker || language_picker) {
                    const int x = ThemePicker::left + 12, w = ThemePicker::width - 24, h = ThemePicker::row_height - 2;
                    const bool chosen = language_picker ? language_choice(i) == settings.language :
                        (i == 0 ? std::string() : font_files[i - 1]) == settings.font_file;
                    if (chosen) {
                        vita2d_draw_rectangle(x + 3, y + 3, w - 6, h - 6, selected);
                        const int bx = x + w - 36;
                        vita2d_draw_rectangle(bx, y + 11, 22, 22, accent);
                        const unsigned check = ui_color::on(accent);
                        for (int line = 0; line < 2; ++line) {
                            vita2d_draw_line(bx + 4, y + 21 + line, bx + 9, y + 26 + line, check);
                            vita2d_draw_line(bx + 9, y + 26 + line, bx + 18, y + 16 + line, check);
                        }
                    }
                    if (i == theme_picker.focused) {
                        vita2d_draw_rectangle(x, y, w, 2, ink);
                        vita2d_draw_rectangle(x, y + h - 2, w, 2, ink);
                        vita2d_draw_rectangle(x, y + 2, 2, h - 4, ink);
                        vita2d_draw_rectangle(x + w - 2, y + 2, 2, h - 4, ink);
                    }
                    draw_text(font, x + 16, y + 28, fit_text(font, language_picker ? language_choice_name(i) : font_name(i), w - 70, 0.65f), ink, 0.65f);
                    continue;
                }
                if (i == theme_picker.focused)
                    skin_box(ui_skin::Slice::MenuItemFocus, ThemePicker::left + 12, y,
                        ThemePicker::width - 24, ThemePicker::row_height - 2, selected, true);
                if (i == settings.theme)
                    vita2d_draw_rectangle(ThemePicker::left + 16, y + 12, 4, 20, accent);
                draw_text(font, ThemePicker::left + 28, y + 28,
                    fit_text(font, theme_name(i), 460, 0.65f), ink, 0.65f);
                std::array<unsigned, 5> pattern{};
                if (ui_skin::theme_colors(i, pattern)) for (size_t c = 0; c < pattern.size(); ++c) {
                    const int x = ThemePicker::left + 510 + (int)c * 32;
                    vita2d_draw_rectangle(x - 1, y + 9, 29, 26, separator);
                    vita2d_draw_rectangle(x, y + 10, 27, 24, pattern[c]);
                }
            }
            if (theme_picker.count > ThemePicker::visible) {
                const float track = ThemePicker::visible * ThemePicker::row_height;
                const float thumb = track * ThemePicker::visible / theme_picker.count;
                const float y = ThemePicker::top + ThemePicker::header +
                    (track - thumb) * theme_picker.first / (theme_picker.count - ThemePicker::visible);
                skin_box(ui_skin::Slice::ScrollThumb, ThemePicker::left + ThemePicker::width - 9, y, 4, thumb, accent);
            }
            if (font_picker) {
                draw_text(font, ThemePicker::left + 12, 492, "ux0:data/vitagram/fonts/  (.ttf / .otf, 8 MiB)", ink, 0.5f);
                draw_text(font, 28, 528, font_pending ? tr("settings.font_loading") :
                    settings_error.empty() ? tr("settings.font_hint") : std::string(settings_error), ink, 0.55f);
            } else draw_text(font, 28, 528, tr("settings.footer"), RGBA8(240,247,255,255), 0.6f);
        }
        if (calendar_open) {
            vita2d_disable_clipping();
            vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0,0,0,170));
            skin_box(ui_skin::Slice::Panel, 250, 80, 460, 424, white, true);
            draw_text(font, 270, 122, "<", ink, 0.8f);
            draw_text(font, 665, 122, ">", ink, 0.8f);
            const auto title = fit_text(font, tr(("month."+std::to_string(calendar.month)).c_str())+" "+std::to_string(calendar.year), 320, 0.65f);
            draw_text(font, 480-rich_text_width(font, 0.65f, title.c_str())/2, 122, title, ink, 0.65f);
            for (int c = 0; c < 7; ++c)
                draw_text(font, 280+c*60, 170, tr(("calendar.weekday."+std::to_string(c)).c_str()), muted, 0.5f);
            for (int d = 1; d <= ChatCalendar::days(calendar.year, calendar.month); ++d) {
                int x = calendar.x(d), y = calendar.y(d);
                const bool enabled = calendar.enabled(d, calendar_today);
                if (d == calendar.day) {
                    if (!ui_skin::nine(ui_skin::Slice::FocusRing, x, y, 54, 38)) {
                        vita2d_draw_rectangle(x, y, 54, 2, accent);
                        vita2d_draw_rectangle(x, y+36, 54, 2, accent);
                        vita2d_draw_rectangle(x, y+2, 2, 34, accent);
                        vita2d_draw_rectangle(x+52, y+2, 2, 34, accent);
                    }
                }
                const std::string label = std::to_string(d);
                draw_text(font, x+27-rich_text_width(font, 0.65f, label.c_str())/2, y+26, label, enabled ? ink : muted, 0.65f);
                if (!enabled) vita2d_draw_line(x+15, y+18, x+39, y+18, muted);
            }
            draw_text(font, 272, 482, fit_text(font, tr("calendar.back"), 416, 0.55f), ink, 0.55f);
            draw_text(font, 28, 530, fit_text(font, tr("calendar.hint"), 904, 0.5f), RGBA8(255,255,255,255), 0.5f);
        }
        vita2d_disable_clipping();
        sceGxmSetViewport(vita2d_get_context(), 480.f, 480.f, 272.f, -272.f, 0.5f, 0.5f);
        if (drawer_draw_width > 0) {
            vita2d_enable_clipping();
            vita2d_set_clip_rectangle(0, chat_view::FolderDrawer::top, drawer_draw_width, 544);
            vita2d_draw_rectangle(0, chat_view::FolderDrawer::top, drawer_draw_width, 544-chat_view::FolderDrawer::top, ui_skin::color("menu", white));
            if (drawer_draw_width > chat_view::FolderDrawer::rail) {
                ui_skin::icon(ui_skin::Icon::Back, 10, 38, 24);
                draw_text(font,40,57,fit_text(font,tr("folder.default"),58,0.48f),ink,0.48f);
                const int count = (int)tg.folders().size();
                for (int row=0; row<chat_view::FolderDrawer::rows; ++row) {
                    const int index = folder_drawer.first+row;
                    if (index>=count) break;
                    const int y = chat_view::FolderDrawer::row_top + row*chat_view::FolderDrawer::row_height;
                    const bool active = tg.folders()[index].id == tg.selectedFolder();
                    skin_box(active ? ui_skin::Slice::TabActive : ui_skin::Slice::Row,6,y,112,48,active ? selected : white,true);
                    if (active && !ui_skin::icon(ui_skin::Icon::Check,91,y+15,18)) {
                        vita2d_draw_line(93,y+24,98,y+29,ink); vita2d_draw_line(98,y+29,107,y+18,ink);
                    }
                    if (folder_drawer.open && index==folder_drawer.focus && !ui_skin::nine(ui_skin::Slice::FocusRing,6,y,112,48)) {
                        vita2d_draw_rectangle(6,y,112,2,accent); vita2d_draw_rectangle(6,y+46,112,2,accent);
                        vita2d_draw_rectangle(6,y+2,2,44,accent); vita2d_draw_rectangle(116,y+2,2,44,accent);
                    }
                    draw_text(font,16,y+29,fit_text(font,tg.folders()[index].title,active ? 70 : 94,0.48f),ink,0.48f);
                }
                if (folder_drawer.first>0) { vita2d_draw_line(102,54,108,48,accent); vita2d_draw_line(108,48,114,54,accent); }
                if (folder_drawer.first+chat_view::FolderDrawer::rows<count) { vita2d_draw_line(53,494,59,500,accent); vita2d_draw_line(59,500,65,494,accent); }
            }
            vita2d_draw_rectangle(drawer_draw_width-12,chat_view::FolderDrawer::top,12,544-chat_view::FolderDrawer::top,ui_skin::color("surface_secondary",white));
            vita2d_draw_rectangle(drawer_draw_width-8,231,4,78,accent);
            vita2d_disable_clipping();
        }
        if (bg_show) {
            const float px = bg_bar.left, py = bg_bar.top();
            const bool playing = bg_media.circle ? video_note_animation.playing() : voice.playing();
            const int64_t duration_us = (int64_t)bg_media.duration * 1000000;
            const int64_t position_us = bg_touch && bg_control == audio_bar::Seek && bg_touch_file == bg_media.file ? bg_seek_target : bg_position();
            const unsigned surface = ui_skin::color("menu", white) | 0xff000000u;
            const unsigned text = ui_color::tone(ink, surface, surface, 4.5), detail = ui_color::tone(muted, surface, surface, 4.5);
            const unsigned active = ui_color::tone(accent, surface, surface, 3.0);
            vita2d_draw_rectangle(px, py, 960-px, audio_bar::Layout::height, surface);
            const auto track = bg_bar.cell(audio_bar::Seek);
            const float fraction = duration_us > 0 ? (float)std::clamp(position_us/double(duration_us), 0.0, 1.0) : 0.f;
            const float loaded = duration_us > 0 ? (float)std::clamp((bg_limit()+300000)/double(duration_us), 0.0, 1.0) : 0.f;
            vita2d_draw_rectangle(track.x, py+6, track.w, 3, separator);
            vita2d_draw_rectangle(track.x, py+6, track.w*loaded, 3, detail);
            vita2d_draw_rectangle(track.x, py+6, track.w*fraction, 3, active);
            vita2d_draw_fill_circle(track.x+track.w*fraction, py+7.5f, 4, active);
            const bool playlist = !bg_media.circle && tg.audioTrack();
            for (int i = 0; i < audio_bar::Count; ++i) {
                const auto control = audio_bar::Control(i);
                const auto cell = bg_bar.cell(control);
                const bool disabled = ((control == audio_bar::Previous || control == audio_bar::Next || control == audio_bar::Shuffle || control == audio_bar::Repeat) && !playlist) ||
                    (control == audio_bar::Speed && bg_media.circle);
                const bool selected_control = (control == audio_bar::Shuffle && playlist && tg.audioShuffle()) ||
                    (control == audio_bar::Repeat && playlist && tg.audioRepeat()) || (control == audio_bar::Volume && !bg_volume);
                const unsigned color = disabled ? mix_color(surface, detail, 0.35f) : selected_control || control == audio_bar::Play ? active : detail;
                if (control != audio_bar::Seek) {
                    draw_audio_control(control, cell.x+20, cell.y+20, playing, bg_volume, color);
                    if (control == audio_bar::Speed) {
                        const int percent = bg_media.circle ? 100 : voice.speed();
                        char speed[16]; std::snprintf(speed, sizeof(speed), percent%100 == 0 ? "%.0f×" : percent%50 == 0 ? "%.1f×" : "%.2f×", percent/100.0);
                        const float size = std::min(0.52f, 0.52f*36/std::max(1,rich_text_width(font,0.52f,speed)));
                        draw_text(font, cell.x+20-rich_text_width(font,size,speed)/2, cell.y+27, speed, color, size);
                    }
                    if (selected_control) vita2d_draw_rectangle(cell.x+12,cell.y+36,16,2,color);
                    if (control == audio_bar::Repeat && playlist && tg.audioRepeat() == 2) draw_text(font,cell.x+16,cell.y+25,"1",color,0.4f);
                }
                if (bg_focus && bg_focus_control == control) {
                    vita2d_draw_rectangle(cell.x,cell.y,cell.w,1,active); vita2d_draw_rectangle(cell.x,cell.y+cell.h-1,cell.w,1,active);
                    vita2d_draw_rectangle(cell.x,cell.y+1,1,cell.h-2,active); vita2d_draw_rectangle(cell.x+cell.w-1,cell.y+1,1,cell.h-2,active);
                }
            }
            draw_text(font, bg_bar.titleX(), py+36, fit_text(font,bg_media.title,bg_bar.titleWidth(),0.62f),text,0.62f);
            std::string hint = tr("player.hint");
            if (bg_autoplay) hint = tr("player.loading");
            else if (!bg_media.circle && !voice.error().empty()) hint = voice.error();
            else if (bg_focus) {
                const char* labels[] = {"player.previous","player.play","player.next","player.seek","player.volume","player.shuffle","player.repeat","player.speed","player.close"};
                hint = tr(labels[bg_focus_control]);
                if (bg_focus_control == audio_bar::Volume) hint += " · ↑/↓";
                if (bg_focus_control == audio_bar::Repeat) hint += ": " + tr(tg.audioRepeat() == 2 ? "player.one" : tg.audioRepeat() == 1 ? "player.all" : "player.off");
                if (bg_focus_control == audio_bar::Shuffle) hint += ": " + tr(tg.audioShuffle() ? "player.on" : "player.off");
                hint += "  ·  × / ○";
            }
            draw_text(font,bg_bar.titleX(),py+59,fit_text(font,hint,bg_bar.titleWidth(),0.44f),detail,0.44f);
            const auto timer = clock_text(position_us)+" / "+clock_text(duration_us);
            const float timer_size = std::min(0.48f,0.48f*120/std::max(1,rich_text_width(font,0.48f,timer.c_str())));
            draw_text(font,626,py+44,timer,text,timer_size);
            if (bg_media.circle) {
                vita2d_draw_fill_circle(bg_media.x, bg_media.y, kBgRadius + 3, accent);
                vita2d_draw_fill_circle(bg_media.x, bg_media.y, kBgRadius, RGBA8(0, 0, 0, 255));
                draw_texture_fit(video_note_animation.texture(), bg_media.x - kBgRadius, bg_media.y - kBgRadius, kBgRadius * 2, kBgRadius * 2);
                if (!playing) {
                    vita2d_draw_fill_circle(bg_media.x, bg_media.y, 16, RGBA8(0, 0, 0, 130));
                    if (!ui_skin::icon(ui_skin::Icon::Play, bg_media.x - 11, bg_media.y - 11, 22, RGBA8(255, 255, 255, 230)))
                        draw_text(font, bg_media.x - 5, bg_media.y + 6, ">", RGBA8(255, 255, 255, 230), 0.6f);
                }
            }
            if (bg_volume_open) {
                const auto popup = bg_bar.popup(), volume_track = bg_bar.volumeTrack(), mute = bg_bar.mute();
                vita2d_draw_rectangle(popup.x,popup.y,popup.w,popup.h,surface);
                vita2d_draw_rectangle(popup.x,popup.y,popup.w,1,active);
                draw_text(font,popup.x+12,popup.y+24,fit_text(font,tr("player.volume")+": "+std::to_string(bg_volume)+"%",208,0.52f),text,0.52f);
                draw_audio_control(audio_bar::Mute,mute.x+20,mute.y+20,false,bg_volume,active);
                vita2d_draw_rectangle(volume_track.x,volume_track.y+15,volume_track.w,3,separator);
                vita2d_draw_rectangle(volume_track.x,volume_track.y+15,volume_track.w*bg_volume/100,3,active);
                vita2d_draw_fill_circle(volume_track.x+volume_track.w*bg_volume/100,volume_track.y+16,5,active);
            }
        }
        draw_dialogs.finish();
        finish_frame();
    }

    vita2d_wait_rendering_done();
    tg.closeBotWebApp();
#if VITAGRAM_WEBKIT
    reset_webapp_view();
    webapp_view.shutdown();
#endif
    photo_view.updateMotion(false, 0, false);
    qr_scanner.stop();
    if (auth_qr_texture) vita2d_free_texture(auth_qr_texture);
    circle_playback.reset();
    circle_recorder.cancel();
    recorder.cancel();
    animation.shutdown();
    video_note_animation.shutdown();
    voice.shutdown();
    textures.stop();
    textures.clear();
    ui_skin::unload();
    rich_text_shutdown();
    ui_skin::unload();
    ui_battery::unload();
    vita2d_free_pgf(font);
    vita2d_fini();
    tg.stopWorker();
    net.shutdown();
    sceAppUtilShutdown();
    if (restart_requested) sceAppMgrLoadExec("app0:eboot.bin", nullptr, nullptr);
    sceKernelExitProcess(0);
    return 0;
}
