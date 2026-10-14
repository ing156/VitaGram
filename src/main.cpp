#include "performance_log.h"
#include "i18n.h"
#include "exit_gesture.h"
#include "telegram_client.h"
#include "history_selection.h"
#include "chat_list_scroll.h"
#include "keyboard.h"
#include "vita_net.h"
#include "media_render.h"
#include "ui_skin.h"
#include "media_load_view.h"
#include "media_seek.h"
#include "text_layout_cache.h"
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

static void rounded_rect(float x, float y, float w, float h, unsigned int color) {
    const float r = 10;
    vita2d_draw_rectangle(x + r, y, w - 2*r, h, color);
    vita2d_draw_rectangle(x, y + r, w, h - 2*r, color);
    vita2d_draw_fill_circle(x+r, y+r, r, color);
    vita2d_draw_fill_circle(x+w-r, y+r, r, color);
    vita2d_draw_fill_circle(x+r, y+h-r, r, color);
    vita2d_draw_fill_circle(x+w-r, y+h-r, r, color);
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
static constexpr int kSearchBarShift = 32;   // search field (26 px) plus a 6 px gap, between the status strip and the folder tabs
static constexpr int kStripTop = 42;   // 23 px strip (18 + 5), 6 px under the header; content below starts at 71 (6 px gap)
static void skin_strip(unsigned int fallback) {
    // Same x and width as the list rows (20..940).
    if (!ui_skin::nine(ui_skin::Slice::Panel, 20, kStripTop, 920, 23)) vita2d_draw_rectangle(20, kStripTop, 920, 23, fallback);
}

// Search field below the status strip (6 px gaps around it); tapping it opens the keyboard.
static void search_bar(vita2d_pgf* font, const std::string& text, unsigned int muted, unsigned int fallback, bool focused = false) {
    skin_row(20, 71, 920, 26, focused, fallback, true);
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
    if (ui_skin::corner_button(false, glow)) return;
    draw_corner_ring(false, dark, glow);
    draw_paperclip(26, 519, 1.55f, RGBA8(0, 0, 0, 90));
    draw_paperclip(25, 518, 1.55f, RGBA8(244, 247, 251, 255));
}

// Three dots, as on the original system menu button.
static void draw_menu_button(bool dark, float glow = 0, float opacity = 1.f) {
    if (ui_skin::corner_button(true, glow, opacity)) return;
    if (opacity < 0.5f) return;
    draw_corner_ring(true, dark, glow);
    for (int i = 0; i < 3; ++i) {
        vita2d_draw_fill_circle(924.f + i * 11 + 1, 521, 3.2f, RGBA8(0, 0, 0, 90));
        vita2d_draw_fill_circle(924.f + i * 11, 520, 3.2f, RGBA8(244, 247, 251, 255));
    }
}

// One entry of the right-hand context menu: label key (text carries the button icon), the physical
// button it stands for, or a special action.
struct ChatMenuItem { const char* label; unsigned int button; int special; int icon = -1; };
// Same geometry as the attach menu: 40 px row pitch, 36 px focus bars inset 4 px, first row 4 px below the frame top.
static constexpr int kMenuRowHeight = 40, kMenuX = 600, kMenuWidth = 320, kMenuBottom = 486, kMenuPad = 8, kMenuFocusHeight = 36, kMenuMaxRows = 10;
struct PostReaction { std::string text; int width; bool chosen; int64_t custom_emoji_id; };
// A link inside one wrapped line: UTF-8 byte range of PostLine::text.
struct LineLink { int begin, end; std::string url; };
struct PostLine {
    std::string text, url;
    std::vector<LineLink> links;   // inline links; url holds the first one for the keyboard
    bool transcript = false;       // belongs to the transcription block (revealed with an animation)
    bool comments = false;         // the comments row of a channel post (text holds its label)
    std::vector<PostReaction> reactions;
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

// Link under a horizontal touch position; falls back to the line's first link, as tapping a line always did.
static std::string post_link_at(vita2d_pgf* font, const PostLine& line, int touch_x, int x = 38) {
    int cursor = x, position = 0;
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
                           TextureCache& textures, bool pause_media) {
    if (line.comments) {
        draw_comments_row(font, (float)x - 8, (float)baseline - 19, 900, (float)post_pitch() - 2, line.text, ink, accent);
        return;
    }
    if (line.reactions.empty()) {
        if (line.links.empty()) { draw_text(font, x, baseline, line.text, line.url.empty() ? ink : accent, 0.68f); return; }
        // Inline links: accent colour with an underline, the rest of the line in the normal text colour.
        int position = 0;
        auto segment = [&](int begin, int end, bool link) {
            if (end <= begin) return;
            const std::string part = line.text.substr(begin, end - begin);
            const int width = rich_text_width(font, 0.68f, part.c_str());
            draw_text(font, x, baseline, part, link ? accent : ink, 0.68f);
            if (link) vita2d_draw_rectangle(x, baseline + 3, width, 1, accent);
            x += width;
        };
        for (const auto& link : line.links) { segment(position, link.begin, false); segment(link.begin, link.end, true); position = link.end; }
        segment(position, (int)line.text.size(), false);
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

// ---- media controls (voice waveform, video control bar, video-note ring) ----
struct Rect4 {
    bool on = false;
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return on && px >= x && px < x + w && py >= y && py < y + h; }
};
static std::string clock_text(int64_t us) {
    const int total = (int)std::max<int64_t>(0, us / 1000000);
    char text[16]; std::snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
    return text;
}
static MediaLoadView video_load_view(const MessageItem& message, bool decoder_ready = false) {
    const auto file = message.animation_stream ? message.animation_stream->snapshot() : download_stream::State::Snapshot{};
    return MediaLoadView::make(std::max(message.animation_size, file.size),
        std::max(message.animation_downloaded, file.prefix), !message.animation_path.empty() || file.complete, decoder_ready);
}
static std::string media_byte_count(int64_t bytes) {
    if (bytes < 1024 * 1024) return tr("unit.kib", {std::to_string(std::max<int64_t>(0, bytes) / 1024)});
    char value[32]; std::snprintf(value, sizeof(value), "%.1f", bytes / 1048576.0);
    return tr("unit.mib", {value});
}
// Use the same panel, icon atlas and progress slices as the rest of the UI.
// Download status stays visible even when fullscreen playback controls fade out.
static void draw_media_loading(vita2d_pgf* font, float x, float y, float w, const MediaLoadView& view,
                               int64_t now, unsigned accent, unsigned ink, unsigned muted,
                               const std::string& opening = "") {
    constexpr float h = 64;
    skin_box(ui_skin::Slice::Panel, x, y, w, h, ui_skin::surface_color(RGBA8(24,40,58,240)));
    if (!ui_skin::icon(ui_skin::Icon::Download, x + 16, y + 12, 24)) {
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
    skin_bar(bx, by, bw, 8, determinate ? view.fraction : 0.f, scale_alpha(muted, 0.4f), accent);
    if (!determinate) {
        const float pulse_x = bx + (bw - bw * 0.2f) * (0.5f + 0.5f * std::sin(now / 400000.0));
        if (!ui_skin::nine(ui_skin::Slice::ProgressFill, pulse_x, by, bw * 0.2f, 8))
            vita2d_draw_rectangle(pulse_x, by, bw * 0.2f, 8, accent);
    }
}
struct ControlBar { Rect4 play, track; };
// Play/pause, progress with a knob and "elapsed / total" in one row, so nothing overlaps at any width:
// the time text takes what it needs on the right and the track fills the space between icon and time.
static ControlBar draw_control_bar(vita2d_pgf* font, float x, float y, float w, float h, int64_t position_us, int64_t duration_us,
                                   bool playing, bool seekable, float scrub, unsigned accent, unsigned ink, unsigned muted, float fade = 1.f) {
    const unsigned int white_fade = RGBA8(255, 255, 255, (int)(255.f * fade));
    accent = scale_alpha(accent, fade); ink = scale_alpha(ink, fade); muted = scale_alpha(muted, fade);
    if (!ui_skin::nine(ui_skin::Slice::Panel, x, y, w, h, scale_alpha(ui_skin::card_tint(false), fade))) {
        rounded_rect(x, y, w, h, RGBA8(0, 0, 0, (int)(150.f * fade)));
    }
    const float icon_size = std::min(28.f, h - 8), cy = y + h / 2;
    if (!ui_skin::icon(playing ? ui_skin::Icon::Pause : ui_skin::Icon::Play, x + 10, cy - icon_size / 2, icon_size, white_fade)) {
        if (playing) { vita2d_draw_rectangle(x + 14, cy - 8, 4, 16, ink); vita2d_draw_rectangle(x + 22, cy - 8, 4, 16, ink); }
        else for (int row = 0; row < 16; ++row) vita2d_draw_rectangle(x + 14, cy - 8 + row, 12 * (1.f - std::fabs(row - 7.5f) / 8.f), 1, ink);
    }
    const std::string text = clock_text(position_us) + " / " + (duration_us > 0 ? clock_text(duration_us) : std::string("--:--"));
    const int text_width = rich_text_width(font, 0.55f, text.c_str());
    draw_text(font, (int)(x + w - 14 - text_width), (int)(cy + 6), text, ink, 0.55f);
    const float track_x = x + 10 + icon_size + 14, track_w = std::max(20.f, w - (track_x - x) - 14 - text_width - 14);
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
    ControlBar bar;
    bar.play = {true, x, y, 10 + icon_size + 8, h};
    bar.track = {true, track_x - 8, y, track_w + 16, h};
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
static int quote_height() { return 12 + scaled_px(34); }
static void draw_reply_quote(vita2d_pgf* font, float x, float y, float w, const MessageItem& message, bool dark, unsigned ink, unsigned accent) {
    const float h = (float)quote_height();
    // The date-chip PNG is a slightly different tone than the message card in both themes; flat fallback otherwise.
    if (!ui_skin::nine(ui_skin::Slice::DateChip, x, y, w, h, RGBA8(255, 255, 255, 235)))
        rounded_rect(x, y, w, h, dark ? RGBA8(26, 58, 92, 255) : RGBA8(222, 238, 250, 255));
    vita2d_draw_rectangle(x + 6, y + 6, 3, h - 12, accent);
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
static constexpr int kCardPad = 8;                           // extra top padding inside message cards
static constexpr int kTimeGap = 4;                           // space between message text and the timestamp row
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
static bool message_has_media(const MessageItem& message) {
    return !message.photos.empty() || message.media_file_id > 0 || message.animation;
}
// Body lines shown in a card (at most two), wrapped exactly as the card draws them.
static std::vector<std::string> message_body_lines(vita2d_pgf* font, const MessageItem& message) {
    auto lines = wrap_text(font, clip(message.text, 2048), 680 - (message_has_media(message) ? 108 : 0));
    if (lines.size() > 2) {
        lines.resize(2);
        lines.back() = fit_text(font, lines.back() + "...", 680 - (message_has_media(message) ? 108 : 0), 0.68f);
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
    return message_body_end(message, line_count) + (message.media_label.empty() ? 0 : 22) + kTimeGap;
}
// The card shrinks to its text: one-line messages are short, two-line ones taller.
static int message_height(vita2d_pgf* font, const MessageItem& message, bool group) {
    if (message.service_message) return 36;
    return message_time_row(message, message_body_lines(font, message).size()) + kCardPad + 17 +
        (group && !message.outgoing ? std::max(20, scaled_px(20)) : 0) + (message.reply_to_id ? quote_height() + 4 : 0) + (message.forwarded ? forward_height() : 0);
}

struct HistoryRow { int index, top, height, day_top; std::string day; };
// Shared by drawing and touch hit testing; date pills are not messages.
static std::vector<HistoryRow> history_layout(vita2d_pgf* font, const std::vector<MessageItem>& messages, int selected, bool group) {
    if (messages.empty()) return {};
    selected = std::max(0, std::min(selected, (int)messages.size()-1));
    int begin = std::max(0, selected-5);
    for (;;) {
        std::vector<HistoryRow> rows;
        int y = 71;
        std::string previous;
        for (int i = begin; i < (int)messages.size(); ++i) {
            std::string day = message_day(messages[i]);
            bool separator = day != previous;
            int height = message_height(font, messages[i], group);
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
    // Transcription uses the same text panel as the message text (same style, padding and scrolling).
    if (!message.transcription.empty()) {
        PostLine header(tr(message.transcription_pending ? "post.transcription_pending" : "post.transcription"), "");
        header.transcript = true;
        result.push_back(std::move(header));
        for (const auto& line : wrap_text(font, message.transcription, 840)) { PostLine entry(line, ""); entry.transcript = true; result.push_back(std::move(entry)); }
    }
    const auto wrapped = wrap_text(font, message.text, 840);
    const auto mapped = map_links_to_lines(message.text, wrapped, message.link_spans);
    for (size_t k = 0; k < wrapped.size(); ++k) {
        const std::string& line = wrapped[k];
        std::string target;
        int matches = 0;
        for (const auto& url : message.links) if (line.find(url) != std::string::npos) { target = url; ++matches; }
        PostLine entry(line, mapped[k].empty() ? (matches == 1 ? target : "") : mapped[k].front().url);
        entry.links = mapped[k];
        result.push_back(std::move(entry));
    }
    // Links drawn inline are not repeated; the rest (e.g. hidden in rich messages) keep their own tappable rows.
    for (const auto& url : message.links) {
        const bool inline_link = std::any_of(message.link_spans.begin(), message.link_spans.end(), [&](const LinkSpan& span) { return span.url == url; });
        if (!inline_link) result.push_back({fit_text(font, tr("hint.open") + url, 840, 0.68f), url});
    }
    if (message.has_comments) {   // last row: below the content and the reactions
        PostLine row(comments_label(message), "");
        row.comments = true;
        result.push_back(std::move(row));
    }
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
// Post content starts below the status strip and the date chip drawn at kPostDateTop.
static constexpr int kPostShift = 12, kPostDateTop = 71;
static constexpr int post_photo_top = 80 + kPostShift;
static constexpr int post_media_top = 80 + kPostShift;
static constexpr int post_media_height = 340;   // tallest media area
// Height of the media area from the real aspect ratio, so wide clips don't leave an empty band below.
static int post_media_height_for(const MessageItem& message) {
    if (message.video_width > 0 && message.video_height > 0 && !message.video_note) {
        const float fitted = message.video_height * std::min(900.0f / message.video_width, (float)post_media_height / message.video_height);
        return std::max(120, std::min(post_media_height, (int)std::lround(fitted)));
    }
    return post_media_height;
}
// The text lives in a framed panel below the media; this is its top edge. Media never sits under the panel.
static int post_panel_top(const MessageItem& message) {
    if (!message.photos.empty()) return 296 + kPostShift;
    if (message.animation || message.sticker)
        return post_media_top + post_media_height_for(message) + (message.video && !message.video_note ? 70 : message.video ? 36 + scaled_px(14) : 30) +
            (message.video && message.animation_file_id > 0 && message.animation_error.empty() && video_load_view(message).downloading ? 72 : 0);
    return 96 + kPostShift;
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
    int width = rich_text_width(font, 0.55f, label.c_str()) + 28;
    skin_box(ui_skin::Slice::DateChip, 480-width/2, kPostDateTop-scroll, width, 22, background, true);
    draw_text(font, 494-width/2, kPostDateTop+16-scroll, label, color, 0.5f);
}
static int post_scroll_max(const MessageItem& message, size_t lines) {
    return std::max(0, post_text_top(message) + std::max(0, (int)lines - 1) * post_pitch() + kPanelBottomPad + 10 - 544);
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
}

// Settings list: language, 3 cache rows... 14 rows in total; the stride leaves room for the footer.
// Settings are a scrolling list laid out like the chat list: 47 px rows from y = 66.
constexpr int kSettingsRows = 21, kEncryptionRows = 5, kEncryptionRow = 11, kSettingsTop = 71, kSettingsRowHeight = 47, kSettingsViewport = 400;

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

int main() {
    pthread_init();
#if !VITAGRAM_PICKER_LOG
    std::remove("ux0:data/vitagram/picker.log"); // file names from the card, written by older versions
#endif
    std::string update_failure; // result of installing a staged update, shown in Settings
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
    int skin_theme = -1;   // theme of the loaded ui_skin textures (-1: none yet)
    vault::load();
    // With the vault on, settings.json is sealed: it is read after the key has been entered, and
    // the key screens use the console language until then.
    if (!vault::enabled()) { settings.load(); diag_log::set_mask(settings.log_mask); }
    else settings.theme = AppSettings::themeHint(settings.theme);   // unlock screens follow the saved theme
    i18n::init("app0:lang", settings.language);
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
    // The proxy policy could not be confirmed (settings expected but unreadable, or unsigned vault
    // state): TDLib is not started until the user saves the proxy settings - with or without a proxy.
    bool network_held = false;
    // Encryption gate: first-run question, key entry, and moving old files into/out of the vault.
    // It runs before the network and TDLib start so no file is changed while TDLib may use it.
    {
        const vault_ui::Env vault_env{font, settings.isDark(), settings.theme};
        vault_ui::run_gate(vault_env);
        if (vault::state_unauthenticated()) {
            // vault.json carried no valid signature (edited, stripped, or from an older version - these
            // cannot be told apart): the caches may have been replaced as well. They are only caches -
            // TDLib rebuilds them - so they are dropped rather than sealed and trusted.
            for (const char* cache : {VITAGRAM_DB_DIR "/ui-cache.json", VITAGRAM_DB_DIR "/ui-startup.json", VITAGRAM_DB_DIR "/media-cache.json"})
                std::remove(cache);
        }
        if (vault::enabled()) {
            settings.load();
            diag_log::set_mask(settings.log_mask);
            i18n::select(settings.language);
            apply_language_font();
            network_held = proxy_policy_unknown(settings);
            if (network_held) vault_ui::show_notice(vault_env, tr("network.held_notice"));
        }
        vault_ui::run_migration(vault_ui::Env{font, settings.isDark(), settings.theme});
    }
    ui_skin::load(settings.theme);
    ui_battery::load(settings.theme);
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
    bool encryption_open = false;
    bool logs_open = false;   // diagnostic-log switches, a sub-screen of the settings
    bool about_open = false;
    int about_offset = 0;
    int about_link = -1;
    bool about_chat_pending = false;
    int proxy_input = -1;
    DownloadFolderPicker folder_picker;
    UploadPicker upload_picker;
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
    constexpr int kSearchTop = 103, kSearchRow = 44, kSearchRows = 9;
    // A transcription appearing in the open post is revealed over ~180 ms (tracked per message).
    int64_t tr_track_id = 0, tr_anim_since = 0;
    bool tr_track_has = false;
    SettingsTextMarquee settings_label_marquee, settings_value_marquee;
    int chat_menu_sel = 0;
    auto menu_first = [](int count, int selected) { return std::clamp(selected - (kMenuMaxRows - 1), 0, std::max(0, count - kMenuMaxRows)); };
    int64_t chat_menu_since = 0;
    bool folder_name_input = false;
    AppSettings proxy_draft = settings;
    if (network_held) { settings_open = proxy_open = true; } // straight to the proxy screen
    QrScanner qr_scanner;
    bool proxy_return_to_screen = false, proxy_imported = false;
    std::string pending_proxy_link;
    std::string pending_web_link;
    bool web_link_proceed = false;
    int web_link_scroll = 0;
    auto open_link = [&](const std::string& url) -> i18n::Text {
        if (url.compare(0, 20, "vitagram:transcribe/") == 0) {
            tg.requestTranscription(std::strtoll(url.c_str() + 20, nullptr, 10));
            return "";
        }
        if (is_proxy_link(url)) { pending_proxy_link = url; return ""; }
        if (tg.openTelegramLink(url)) return "";
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
    auto language_label = [&]() {
        auto name_of = [](const std::string& code) {
            for (const auto& language : i18n::languages()) if (language.code == code) return language.name;
            return code;
        };
        return language_pending ? tr("settings.language_loading") :
            settings.language.empty() ? tr("settings.language_auto", {name_of(i18n::current())}) : name_of(settings.language);
    };
    int64_t next_cache_cleanup = sceKernelGetSystemTimeWide() + 300000000;
    TextureCache textures;
    AnimationPlayer animation;
    VoicePlayer voice;
    AudioRecordingDialog<AudioRecorder, VoicePlayer> recorder;
    using AudioRecordingState = decltype(recorder)::State;
    i18n::Text audio_error;
    int64_t audio_chat = 0;
    int32_t audio_topic = 0;
    VideoNoteRecorder circle_recorder;
    std::unique_ptr<AnimationPlayer> circle_playback;
    int64_t circle_chat = 0;
    int32_t circle_topic = 0;
    i18n::Text circle_error;
    int chat_index = 0;
    ChatListScroll chat_scroll, settings_scroll;
    bool chat_drag = false;
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
    media_seek::TimeSelection seek_time;
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

    auto invalidate_spoiler_view = [&](bool changed, bool returning_to_app = false) {
        if (!changed && !returning_to_app) return;
        spoiler_prompt = false;
        if (changed || returning_to_app) {
            animation.setSource("", false, false); tg.setAnimationTarget(-1);
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
        animation.setSource(""); voice.setSource("");
        recorder.open(std::string(VITAGRAM_FILES_DIR) + "/voice-message-" + std::to_string(sceKernelGetSystemTimeWide()) + ".ogg");
    };
    auto open_circle = [&]() {
        if (recorder.active()) {
            browser_error = i18n::text("rec.finish_first"); return;
        }
        if (!tg.ready() || !tg.currentChatId()) { browser_error = i18n::text("rec.wait_connect"); return; }
        circle_chat = tg.currentChatId(); circle_topic = tg.currentTopicId(); circle_error.clear();
        circle_playback.reset();
        animation.setSource(""); voice.setSource("");
        circle_recorder.open(std::string(VITAGRAM_FILES_DIR) + "/video-note-" +
            std::to_string(sceKernelGetSystemTimeWide()) + ".mp4");
    };

    bool startup_ready_measured = false;
    performance::observe(performance::Metric::StartupUi, performance::now()-startup_time);
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
            else v = {{"menu.send_sticker", SCE_CTRL_CROSS, 0}, {"menu.sets", SCE_CTRL_CIRCLE, 0}, {"menu.close_picker", SCE_CTRL_SQUARE, 0}};
        } else if (reactions_open) {
            v = {{"menu.react_toggle", SCE_CTRL_CROSS, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
        } else if (video_fullscreen) {
            if (animation.readyForPlayback()) v.push_back({"menu.play_pause", SCE_CTRL_CROSS, 0});
            if (animation.canSeek()) {
                v.push_back({"menu.seek_back", SCE_CTRL_LTRIGGER, 0}); v.push_back({"menu.seek_forward", SCE_CTRL_RTRIGGER, 0});
                if (has_message && m[message_index].video_duration > 0) v.push_back({"menu.seek_time", 0, 40});
            }
            v.push_back({"menu.normal_view", SCE_CTRL_TRIANGLE, 0}); v.push_back({"menu.back", SCE_CTRL_CIRCLE, 0});
        } else if (photo_open) {
            v = {{"menu.prev_photo_dpad", SCE_CTRL_LEFT, 0}, {"menu.next_photo_dpad", SCE_CTRL_RIGHT, 0},
                 {"menu.rotate_left", SCE_CTRL_LTRIGGER, 0}, {"menu.rotate_right", SCE_CTRL_RTRIGGER, 0},
                 {"menu.reset_zoom", SCE_CTRL_TRIANGLE, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
        } else if (post_open && has_message && m[message_index].poll.id) {
            v = {{"menu.poll_choose", SCE_CTRL_CROSS, 0}, {"menu.poll_send", SCE_CTRL_TRIANGLE, 0},
                 {"menu.poll_retract", SCE_CTRL_SQUARE, 0}, {"menu.back", SCE_CTRL_CIRCLE, 0}};
        } else if (post_open && has_message) {
            const MessageItem& item = m[message_index];
            v = {{"menu.prev_post", SCE_CTRL_LEFT, 0}, {"menu.next_post", SCE_CTRL_RIGHT, 0}};
            if (item.photos.size() > 1) { v.push_back({"menu.prev_photo", SCE_CTRL_LTRIGGER, 0}); v.push_back({"menu.next_photo", SCE_CTRL_RTRIGGER, 0}); }
            if (item.animation && spoiler_locked(item)) v.push_back({"spoiler.title", SCE_CTRL_CROSS, 0});
            if (!item.photos.empty()) v.push_back({"menu.open_photo", SCE_CTRL_CROSS, 0});
            if (item.voice || (item.video && (animation.readyForPlayback() || animation.preparationRequired()))) v.push_back({"menu.play_pause", SCE_CTRL_CROSS, 0});
            if ((item.voice || (item.video && animation.canSeek())) && item.photos.empty()) {
                v.push_back({"menu.seek_back", SCE_CTRL_LTRIGGER, 0}); v.push_back({"menu.seek_forward", SCE_CTRL_RTRIGGER, 0});
            }
            if ((item.voice && !item.voice_path.empty() && item.voice_error.empty() && item.voice_duration > 0) ||
                (item.video && animation.canSeek() && item.video_duration > 0)) v.push_back({"menu.seek_time", 0, 40});
            if (item.video && !item.video_note) v.push_back({"menu.fullscreen", SCE_CTRL_TRIANGLE, 0});
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
        if (has_message && !photo_open && !video_fullscreen && !stickers_open && !gifs_open && !reactions_open &&
            !circle_recorder.active() && !recorder.active() && !tg.topicsOpen() && tg.canDeleteMessage(m[message_index].id, false))
            v.push_back({"menu.delete_message", 0, 34, (int)ui_skin::Icon::Trash});
        return v;
    };
    auto attach_menu_items = [&]() {
        std::vector<ChatMenuItem> items = {{"upload.image", 0, 0}, {"upload.video", 0, 1},
            {"upload.file", 0, 2, (int)ui_skin::Icon::File}, {settings.recording_shortcuts ? "upload.video_note_shortcut" : "upload.video_note", 0, 3},
            {settings.recording_shortcuts ? "upload.voice_message_shortcut" : "upload.voice_message", 0, 4}};
        if (tg.canPostCurrent()) {
            items.push_back({"gif.menu", 0, 5, (int)ui_skin::Icon::Search});
            items.push_back({"menu.stickers", 0, 6});
        }
        const auto& messages = tg.messages();
        if (message_index >= 0 && message_index < (int)messages.size() && !messages[message_index].service_message)
            items.push_back({"reaction.title", 0, 7});
        return items;
    };
    constexpr int attach_menu_bottom = 486, attach_menu_row = 40;
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
        } else if (m.video && m.video_duration > 0 && animation.canSeek()) {
            const int64_t limit = std::max<int64_t>(0, (int64_t)m.video_duration * 1000000 - 500000);
            animation.seekTo(std::max<int64_t>(0, std::min<int64_t>(limit, animation.positionUs() + delta_us)));
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
            } else if (animation.canSeek()) {
                if (apply) animation.seekTo(seek_time.targetUs());
                if (animation.playing() != seek_was_playing) animation.toggle();
            }
        }
        seek_open = false; seek_touching = false;
    };
    auto open_seek = [&]() {
        const auto& messages = tg.messages();
        if (message_index < 0 || message_index >= (int)messages.size()) return;
        const auto& message = messages[message_index];
        const bool audio = message.voice && !message.voice_path.empty() && message.voice_error.empty() && message.voice_duration > 0;
        if (!audio && !(message.video && animation.canSeek() && message.video_duration > 0)) return;
        seek_chat = tg.currentChatId(); seek_message = message.id; seek_audio = audio;
        seek_was_playing = audio ? voice.playing() : animation.playing();
        seek_time.begin(audio ? (int64_t)(voice.positionSeconds() * 1000000) : animation.positionUs(),
            (int64_t)(audio ? message.voice_duration : message.video_duration) * 1000000, audio ? 300000 : 500000);
        if (audio) voice.setPlaying(false);
        else if (seek_was_playing) animation.toggle();
        seek_open = true; seek_touching = false; touch_down = false; scrub_kind = 0;
    };
    // Corner buttons exist on every chat screen except modal tools (recorder, scanner, file picker).
    auto chat_ui_visible = [&]() {
        return (tg.currentChatId() != 0 || tg.topicsOpen() || tg.canBrowse()) && !settings_open &&
               !qr_scanner.active() && !kb.active() && !seek_open && !unsupported_video_prompt && !spoiler_prompt && pending_web_link.empty() &&
               (upload_picker.stage == UploadPicker::Stage::Closed || upload_picker.stage == UploadPicker::Stage::Menu);
    };
    // Vault auto-lock. Wall-clock time keeps running while the console sleeps or the app sits behind the
    // LiveArea, so a jump between two frames means the user left; ON_RESUME also catches short sleeps.
    constexpr uint64_t kLeftAppUs = 10000000;
    auto wall_us = []() -> uint64_t { SceRtcTick tick{}; sceRtcGetCurrentTick(&tick); return tick.tick; };
    uint64_t lock_previous_frame = 0, lock_last_activity = wall_us();
    int64_t next_video_power_tick = 0;
    bool lock_pending = false; // left the app while the system keyboard was open: lock once it closes
    auto lock_app = [&]() {
        seek_open = false; seek_touching = false;
        // Covering the screen is enough: TDLib, downloads and conversions keep running underneath.
        if (animation.playing()) animation.toggle();
        if (circle_playback && circle_playback->playing()) circle_playback->toggle();
        if (voice.playing()) voice.toggle();
        recorder.pausePreview();
        vault_ui::relock(vault_ui::Env{font, settings.isDark(), settings.theme},
                         [](void* client) { static_cast<decltype(&tg)>(client)->pump(false); cpu_scheduler::tick(); }, &tg);
        SceCtrlData after{}; sceCtrlPeekBufferPositive(0, &after, 1); old_buttons = after.buttons;
        vita2d_set_clear_color(settings.isDark() ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255));
        lock_previous_frame = lock_last_activity = wall_us();
    };
    while (running) {
        performance::Scope frame_measurement(performance::Metric::Frame);
        performance::Scope ui_measurement(performance::Metric::UiWork);
        performance::SlowScope slow_ui(performance::SlowPhase::Ui,"frame_ui");
        performance::SlowScope pre_input(performance::SlowPhase::Ui,"pre_input");
        int64_t now = sceKernelGetSystemTimeWide();
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
            if (left_app && settings.spoilerHideApp()) {
                invalidate_spoiler_view(spoiler_visibility.appLeft(true), true);
            }
            lock_previous_frame = wall;
            const uint64_t idle_limit = (uint64_t)AppSettings::lockMinutes(settings.auto_lock) * 60000000ULL;
            // Recording, playback and typing in the system keyboard are activity without button presses.
            const auto circle_state = circle_recorder.state();
            const bool busy = recorder.recording() || recorder.processing() || recorder.previewPlaying() || animation.playing() || voice.playing() || kb.active() ||
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
        const bool button_action = pressed != 0;
        SceTouchData touch{};
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        const bool interaction = button_action || touch.reportNum || touch_down || (old_buttons & ~pad.buttons);
        if (interaction) media_active_at = now;
        if (interaction) lock_last_activity = wall_us();
        const int64_t input_sampled = interaction ? performance::now() : 0;
        if (interaction) tg.prioritizeInteraction(now);
        unsigned int released = old_buttons & ~pad.buttons;
        old_buttons = pad.buttons;
        bool compose_context = !circle_recorder.active() && !recorder.active() && tg.currentChatId() && !tg.topicsOpen() && !post_open && !photo_open && !settings_open &&
            !reactions_open && !stickers_open && !gifs_open && !upload_picker.active() && !kb.active() && !recorder.recording() && !recorder.processing() && tg.inputKind() == InputKind::None;
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
        observe_spoiler_chat();
        if (tg.takeLinkChatOpened()) {
            if (about_chat_pending) {
                settings_open = about_open = false; about_chat_pending = false;
                settings_error.clear();
            }
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
        if (gifs_open && (!tg.ready() || !tg.gifsCurrent())) {
            gifs_open = false; tg.closeGifs();
            if (gif_input) { kb.close(); gif_input = false; }
        }
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
        else if (history_selection.message_id && post_open) {
            post_open = photo_open = video_fullscreen = reactions_open = false;
            animation.setSource(""); voice.setSource("");
        }
        if (was_at_bottom && !post_open && !photo_open && !tg.messages().empty() &&
            tg.messages().back().outgoing && tg.messages().back().id != previous_last)
            message_index = (int)tg.messages().size() - 1;
        if (const int64_t target = tg.takeMessageJump()) {
            for (size_t i = 0; i < tg.messages().size(); ++i) if (tg.messages()[i].id == target) {
                message_index = (int)i;
                post_open = tg.messageJumpOpensPost(); photo_open = video_fullscreen = reactions_open = false;
                photo_index = 0; post_text_scroll = 0;
                animation.setSource(""); voice.setSource("");
                break;
            }
        }

        const bool folder_tabs = !tg.folders().empty();
        const int chat_rows = folder_tabs ? 9 : 10;
        const int chat_top = (folder_tabs ? 119 : 71) + kSearchBarShift;   // below the status strip, the search field and the tabs
        const int chat_row_height = (544-chat_top)/chat_rows;
        chat_scroll.configure((int)tg.chats().size(), chat_row_height, 544-chat_top);
        settings_scroll.configure(encryption_open ? kEncryptionRows : kSettingsRows, kSettingsRowHeight, kSettingsViewport);
        const int folder_max_scroll = std::max(0, (int)tg.folders().size() * 180 - 920);
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
        photo_view.updateMotion(photo_open && settings.gyroscope_enabled, sceKernelGetProcessTimeWide(), touch.reportNum != 0);
        bool photo_gesture = photo_open && photo_view.touch(touch);
        bool touching = touch.reportNum > 0;
        bool tap = false;
        if (touching) {
            touch_x = touch.report[0].x / 2;
            touch_y = touch.report[0].y / 2;
            if (!touch_down) {
                touch_start_x = touch_x; touch_start_y = touch_step_y = touch_y;
                touch_dragged = false;
                scrub_kind = 0;
                touch_blocked = kb.active() || (!circle_recorder.active() && !recorder.active() && !settings_open && tg.canBrowse() && tg.inputKind() != InputKind::None);
                folder_drag = !search_open && pending_web_link.empty() && !circle_recorder.active() && !recorder.active() && !stickers_open && !gifs_open && !settings_open && folder_tabs && !tg.currentChatId() && touch_start_y >= 69 + kSearchBarShift && touch_start_y < 113 + kSearchBarShift;
                folder_touch_x = touch_x;
                if (!touch_blocked && !chat_menu_open && !upload_picker.active() && delete_hit.contains(touch_x, touch_y)) {
                    scrub_kind = 6; delete_touch_message = delete_hit_message; delete_touch_chat = tg.currentChatId();
                }
                else if (!touch_blocked && reply_hit.contains(touch_x, touch_y)) scrub_kind = 5;
                else if (!touch_blocked && scrub_play.contains(touch_x, touch_y)) scrub_kind = 4;
                else if (!touch_blocked && scrub_bar.contains(touch_x, touch_y)) scrub_kind = 2;
                else if (!touch_blocked && scrub_voice.contains(touch_x, touch_y)) scrub_kind = 1;
                else if (!touch_blocked && scrub_ring.on) {
                    const float distance = std::hypot(touch_x - scrub_ring.x, touch_y - scrub_ring.y);
                    if (distance >= scrub_ring.w - 28 && distance <= scrub_ring.w + 24) scrub_kind = 3;
                }
                chat_drag = !search_open && pending_web_link.empty() && !circle_recorder.active() && !recorder.active() && !stickers_open && !gifs_open && !settings_open &&
                    !reactions_open && !upload_picker.active() && !tg.currentChatId() && tg.canBrowse() &&
                    !touch_blocked && touch_start_y >= chat_top && touch_start_y < 544;
                chat_touch_y = touch_y;
            }
            if (std::abs(touch_y - touch_start_y) > 14 || std::abs(touch_x - touch_start_x) > 14) touch_dragged = true;
            if (scrub_kind >= 1 && scrub_kind <= 3) {
                // The finger sets the position: along the bar/waveform, or by angle around the ring (clockwise from the top).
                float fraction = 0;
                if (scrub_kind == 1) fraction = (touch_x - 184.f) / 592.f;
                else if (scrub_kind == 2) fraction = (touch_x - (scrub_bar.x + 8)) / std::max(1.f, scrub_bar.w - 16);
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
                chat_scroll.drag(touch_y - chat_touch_y);
                if (chat_scroll.end() > chat_scroll.first())
                    chat_index = std::clamp(chat_index, chat_scroll.first(), chat_scroll.end()-1);
                if (chat_scroll.end() >= (int)tg.chats().size() - 2 && touch_y < chat_touch_y)
                    tg.loadMoreChats();
            }
            // Include the initial movement once the tap threshold is crossed.
            if (chat_drag && touch_dragged) chat_touch_y = touch_y;
            if (!chat_drag && !folder_drag && !touch_blocked && !scrub_kind && touch_start_y >= 38 && touch_start_y < 544 && !photo_open && !video_fullscreen &&
                std::abs(touch_y - touch_start_y) >= std::abs(touch_x - touch_start_x) &&
                std::abs(touch_y - touch_step_y) >= 32) {
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
                } else if (control != 1 && target.video && target.video_duration > 0 && animation.canSeek()) {
                    const int64_t limit = std::max<int64_t>(0, (int64_t)target.video_duration * 1000000 - 500000);
                    const int64_t wanted = std::min<int64_t>(limit, (int64_t)(scrub_fraction * target.video_duration * 1e6));
                    if (std::llabs(wanted - animation.positionUs()) > 250000) animation.seekTo(wanted);
                }
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
        textures.beginFrame(interaction || decode_cooldown != 0 || kb.active() || circle_recorder.active() || recorder.active());

        post_pump.finish();
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

        if (!pending_web_link.empty()) {
            if (pressed & SCE_CTRL_LEFT) web_link_proceed = false;
            if (pressed & SCE_CTRL_RIGHT) web_link_proceed = true;
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
            const bool ui = chat_ui_visible();
            auto open_menu = [&]() {
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
                else if (item.special == 40) open_seek();
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
            if (chat_menu_waiting && tap && std::hypot(float(touch_x)-960.f, float(touch_y)-544.f) >= 66.f) chat_menu_waiting = false;
            if (chat_menu_waiting && (!tg.deletePermissionPending(chat_menu_message) || now - chat_menu_wait_since >= 2000000)) {
                chat_menu_waiting = false; chat_menu_open = true; chat_menu_sel = 0; chat_menu_since = now;
                chat_menu_snapshot = chat_menu_items();
            }
            if (ui && (tg.currentChatId() != 0 || tg.topicsOpen()) && (pressed & SCE_CTRL_START) && !(pad.buttons & SCE_CTRL_SELECT)) {
                if (chat_menu_open || chat_menu_waiting) { chat_menu_open = false; chat_menu_waiting = false; } else open_menu();
                pressed &= ~SCE_CTRL_START;
            } else if (ui && chat_menu_open) {
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
                const bool in_right = std::hypot((float)touch_x - 960.0f, (float)touch_y - 544.0f) < 66.0f;
                const bool in_left = std::hypot((float)touch_x, (float)touch_y - 544.0f) < 66.0f;
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
                    const int body_top = kMenuBottom - std::min(kMenuMaxRows, (int)items.size()) * kMenuRowHeight - 2 * kMenuPad, top = body_top + kMenuPad;
                    const bool inside = touch_x >= kMenuX && touch_x < kMenuX + kMenuWidth && touch_y >= body_top && touch_y < kMenuBottom;
                    if (inside && touch_y >= top && touch_y < kMenuBottom - kMenuPad) run_item(items[std::min((int)items.size() - 1, first + (touch_y - top) / kMenuRowHeight)]);
                    else if (!inside) chat_menu_open = false;
                    tap = false;
                }
            }
        }
        if (tap && !kb.active() && (recorder.active() || circle_recorder.active() || settings_open || !tg.canBrowse() || tg.inputKind() == InputKind::None)) {
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
            } else if (!settings_open && tg.topicsOpen()) {
                if (touch_y >= 500) {}
                else if (touch_y >= 70 && touch_y < 470) {
                    int index = std::max(0, topic_index-7)+(touch_y-70)/50;
                    if (index < (int)tg.topics().size()) { topic_index = index; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (upload_picker.active()) {
                if (upload_picker.stage == UploadPicker::Stage::Menu) {
                    const int top = attach_menu_bottom - (int)attach_menu_items().size() * attach_menu_row - kMenuPad;
                    if (touch_x >= 40 && touch_x < 340 && touch_y >= top && touch_y < attach_menu_bottom - kMenuPad) {
                        upload_picker.selected = (touch_y - top) / attach_menu_row; pressed |= SCE_CTRL_CROSS;
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
                else if (touch_y >= 170 && touch_y < 445) {
                    int index = poll_index / 5 * 5 + (touch_y - 170) / 55;
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
                    if (hit >= 0) { settings_row = hit; pressed |= SCE_CTRL_CROSS; }
                }
            } else if (!tg.currentChatId() && !tg.canBrowse()) {
                if ((touch_x >= 30 && touch_x < 470 && touch_y >= 280 && touch_y < 340 && tg.qrLink().empty()) ||
                    (touch_y >= 500 && touch_x >= 480)) pressed |= SCE_CTRL_START;
                else if (touch_y >= 500 && touch_x < 480) pressed |= SCE_CTRL_CROSS;
            } else if (!tg.currentChatId() && tg.canBrowse() && folder_tabs && touch_y >= 69 && touch_y < 113 && touch_x >= 20 && touch_x < 940) {
                int index = (touch_x - 20 + folder_scroll) / 180;
                if (index < (int)tg.folders().size()) { tg.selectFolder(tg.folders()[index].id); chat_index = 0; chat_scroll.reset(); }
            } else if (!tg.currentChatId() && tg.canBrowse() && touch_y >= 42 && touch_y < 60) {
                tg.loadMoreChats();
            } else if (tg.currentChatId() && tg.ready() && !post_open && !photo_open && !tg.topicsOpen() &&
                       std::hypot((float)touch_x, (float)touch_y - 544.0f) < 66.0f) {
                upload_picker.openMenu(); chat_menu_open = false;
            } else if (touch_y >= 500 && !tg.currentChatId() && !tg.canBrowse()) {
                pressed |= touch_x >= 650 ? SCE_CTRL_TRIANGLE : SCE_CTRL_CROSS;
            } else if (tg.currentChatId() && !post_open && !photo_open && touch_y >= 71 && touch_y < 544) {
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
                    for (const auto& row : history_layout(font, tg.messages(), message_index, tg.currentChatIsGroup()))
                        if (touch_y >= row.top && touch_y < row.top + row.height - 4) {
                            message_index = row.index; pressed |= SCE_CTRL_CROSS; break;
                        }
            } else if (post_open && !photo_open && message_index >= 0 && message_index < (int)tg.messages().size()) {
                const auto& post = tg.messages()[message_index];
                if (scrollable_post(post)) {
                    if (touch_y >= 40 && touch_y < 544) {
                        if (post.video && touch_x >= 30 && touch_x < 930 &&
                            touch_y >= post_media_top-post_text_scroll &&
                            touch_y < post_media_top+post_media_height_for(post)-post_text_scroll) {
                            pressed |= SCE_CTRL_CROSS;
                        } else if (!post.photos.empty() && touch_y >= post_photo_top-post_text_scroll && touch_y < post_photo_top+170-post_text_scroll) {
                            for (const auto& tile : photo_carousel(post, photo_index))
                                if (touch_x >= tile.x && touch_x < tile.x+tile.width) {
                                    photo_index = tile.index; pressed |= SCE_CTRL_CROSS; break;
                                }
                        } else {
                            auto lines = post_lines(font, post);
                            int relative = touch_y - (post_text_top(post)-19-post_text_scroll);
                            int line = relative / post_pitch();
                            if (relative >= 0 && line < (int)lines.size() && lines[line].comments) open_comments();
                            else if (relative >= 0 && line < (int)lines.size() && !lines[line].url.empty())
                                browser_error = open_link(post_link_at(font, lines[line], touch_x));
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
                int text_y = tg.messages()[message_index].voice ? 346 + post_quote_extra(tg.messages()[message_index]) : (tg.messages()[message_index].sticker || tg.messages()[message_index].animation) ? 330 : (count == 0 ? 110 : (count > 2 ? 470 : 330));
                if ((tg.messages()[message_index].voice || tg.messages()[message_index].video) && touch_x >= 30 && touch_x < 930 && touch_y >= post_media_top-post_text_scroll && touch_y < post_media_top+post_media_height_for(tg.messages()[message_index])-post_text_scroll)
                    pressed |= SCE_CTRL_CROSS;
                if (touch_x >= 38 && touch_x < 930 && touch_y >= text_y - 19 && touch_y < 544) {
                    auto lines = post_lines(font, tg.messages()[message_index]);
                    int line = post_text_scroll + (touch_y - (text_y - 19)) / 24;
                    if (line >= 0 && line < (int)lines.size() && lines[line].comments) open_comments();
                    else if (line >= 0 && line < (int)lines.size() && !lines[line].url.empty())
                        browser_error = open_link(post_link_at(font, lines[line], touch_x));
                }
                if (touch_x >= x0 && touch_x < (columns == 2 ? 912 : 800) && touch_y >= 90 && touch_y < text_y - 19 &&
                    row >= 0 && row < (count > 2 ? 3 : 1) && col < columns && index < count) {
                    photo_index = index; pressed |= SCE_CTRL_CROSS;
                }
                }
            } else if (!tg.currentChatId() && tg.canBrowse() && touch_y >= 66 && touch_y < kSearchTop - 3 && (search_open || touch_y < 71 + kSearchBarShift - 3)) {
                pressed |= SCE_CTRL_SQUARE;   // the search field
            } else if (search_open && !tg.currentChatId() && tg.canBrowse()) {
                const int index = search_first + (touch_y - kSearchTop) / kSearchRow;
                if (touch_y >= kSearchTop && index >= 0 && index < (int)tg.searchResults().size() && index < search_first + kSearchRows) {
                    search_index = index; pressed |= SCE_CTRL_CROSS;
                }
            } else if (!tg.currentChatId() && tg.canBrowse() && touch_y >= chat_top && touch_y < 544) {
                int index = chat_scroll.hit(touch_y - chat_top);
                if (index >= 0 && index < (int)tg.chats().size()) { chat_index = index; search_bar_focus = false; pressed |= SCE_CTRL_CROSS; }
            }
        }

        if (tg.currentChatId() != 0 && !tg.messages().empty()) {
            if (message_index < 0) message_index = (int)tg.messages().size() - 1;
            message_index = std::max(0, std::min(message_index, (int)tg.messages().size() - 1));
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
            kb.open(tg.inputPrompt(), tg.inputSecret());
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
                if (!shown[message_index].photos.empty()) {
                    tg.ensureFullPhoto((size_t)message_index, (size_t)photo_index);
                    photo_open = true; photo_view.begin();
                } else if (shown[message_index].video) toggle_video = true;
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
        if ((pressed & SCE_CTRL_START) && !(pad.buttons & SCE_CTRL_SELECT) && !kb.active() && !qr_scanner.active() && !circle_recorder.active() && !recorder.active() && !tg.currentChatId()) {
            bool auth_proxy = !settings_open && !tg.canBrowse();
            settings_open = !settings_open;
            proxy_open = auth_proxy;
            logs_open = false;
            encryption_open = false;
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
            kb.handle(pressed);
        } else if (kb.submitted()) {
            std::string v = kb.text();
            kb.close();
            if (gif_input) {
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
            if (gif_input) gif_input = false;
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
            const int rows = upload_picker.stage == Stage::Menu ? (int)attach_menu_items().size() : upload_picker.stage == Stage::Review ? upload_picker.reviewRows() : (int)upload_picker.entries.size();
            if (rows > 0) {
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
                    if (action < 3) upload_picker.chooseKind((UploadPicker::Kind)action);
                    else {
                        upload_picker.close();
                        if (action == 3) open_circle();
                        else if (action == 4) record_toggle = true;
                        else if (action == 5) { gifs_open = true; gif_index = 0; gif_next_page = -1; tg.searchGifs(); }
                        else if (action == 6) { stickers_open = true; sticker_pack_menu = true; sticker_index = 0; sticker_pack = 1; tg.openStickers(); }
                        else if (action == 7 && message_index >= 0 && message_index < (int)tg.messages().size()) {
                            reactions_open = true; reaction_index = 0;
                            tg.loadReactions(tg.messages()[message_index].id);
                        }
                    }
                }
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
        } else if (!settings_open && gifs_open) {
            const int count = (int)tg.gifs().items.size();
            if (pressed & SCE_CTRL_CIRCLE) { gifs_open = false; tg.closeGifs(); }
            else if (pressed & SCE_CTRL_SQUARE) {
                gif_input = true; kb.open(tr("gif.kb_title"), false, tg.gifs().query);
            } else if (pressed & SCE_CTRL_TRIANGLE) { gif_index = 0; gif_next_page = -1; tg.searchGifs(); }
            else {
                if (pressed & SCE_CTRL_LEFT) --gif_index;
                if (pressed & SCE_CTRL_RIGHT) ++gif_index;
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
                settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                folder_picker.active = false;
            }
        } else if (settings_open && about_open) {
            const auto& layout = about_layout(font);
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
                about_open = false; about_chat_pending = false; settings_row = 17; settings_error.clear();
            } else if ((pressed & SCE_CTRL_CROSS) && about_link >= 0 && layout.link_visible(about_link, about_offset)) {
                if (about_link == 0) browser_error = open_link(kAboutProjectUrl);
                else if (!tg.ready()) settings_error = i18n::text("about.telegram_unavailable");
                else { about_chat_pending = true; browser_error = open_link(kAboutAuthorUrl); settings_error.clear(); }
            }
        } else if (settings_open && logs_open) {
            if (pressed & SCE_CTRL_CIRCLE) { logs_open = false; settings_row = 16; settings_error.clear(); }
            else {
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
                else if (encryption_open) {
                    encryption_open = false; settings_row = kEncryptionRow; settings_error.clear();
                    settings_scroll.configure(kSettingsRows, kSettingsRowHeight, kSettingsViewport);
                    settings_scroll.reset(); settings_scroll.reveal(settings_row); pressed = 0;
                } else settings_open = false;
            }
            const int count = encryption_open ? kEncryptionRows : kSettingsRows;
            if (pressed & SCE_CTRL_UP) { settings_row = (settings_row+count-1)%count; confirm_logout = false; confirm_vault_off = false; }
            if (pressed & SCE_CTRL_DOWN) { settings_row = (settings_row+1)%count; confirm_logout = false; confirm_vault_off = false; }
            settings_scroll.reveal(settings_row);
            if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) {
                int direction = (pressed & SCE_CTRL_LEFT) ? -1 : 1;
                // Row 0 is the language; the remaining rows keep their original order.
                const int source_row = encryption_open ? kEncryptionRow + settings_row :
                    settings_row > kEncryptionRow ? settings_row + kEncryptionRows - 1 : settings_row;
                const int row = source_row - 1;
                if (!encryption_open && settings_row == kEncryptionRow) {
                    if (pressed & SCE_CTRL_CROSS) {
                        encryption_open = true; settings_row = 0; settings_scroll.reset();
                        settings_error.clear(); confirm_logout = confirm_vault_off = false;
                    }
                } else if (!encryption_open && settings_row == 0) {
                    confirm_logout = false;
                    // Automatic first, then every language file found in app0:lang.
                    std::vector<std::string> choices{""};
                    for (const auto& language : i18n::languages()) choices.push_back(language.code);
                    size_t at = std::find(choices.begin(), choices.end(), language_pending ? requested_language : settings.language) - choices.begin();
                    if (at >= choices.size()) at = 0;
                    requested_language = choices[(at + choices.size() + direction) % choices.size()];
                    language_pending = true;
                    // Invalidate an older read before the between-frame poll adopts it.
                    rich_text_prepare_language_font(language_font_path_for(i18n::resolve(requested_language)));
                    settings_error.clear();
                } else if (row < 4) {
                    confirm_logout = false;
                    int* values[] = {&settings.retention, &settings.capacity, &settings.theme, &settings.font};
                    const int counts[] = {5, 6, AppSettings::kThemeChoices, 7};
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
                    if (pressed & SCE_CTRL_CROSS) { proxy_open = true; proxy_return_to_screen = proxy_imported = false; proxy_draft = settings; settings_row = 0; settings_error.clear(); confirm_logout = false; }
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
                        const vault_ui::Env vault_env{font, settings.isDark(), settings.theme};
                        auto finish_vault_flow = [&](vault_ui::Outcome outcome, bool restart) {
                            // Buttons used in the dialog must not leak into the main screen.
                            SceCtrlData after{}; sceCtrlPeekBufferPositive(0, &after, 1); old_buttons = after.buttons; pressed = 0;
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
                            const vault_ui::Env vault_env{font, settings.isDark(), settings.theme};
                            auto outcome = vault_ui::change_flow(vault_env);
                            SceCtrlData after{}; sceCtrlPeekBufferPositive(0, &after, 1); old_buttons = after.buttons; pressed = 0;
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
                            vault_ui::check_flow(vault_ui::Env{font, settings.isDark(), settings.theme});
                            SceCtrlData after{}; sceCtrlPeekBufferPositive(0, &after, 1); old_buttons = after.buttons; pressed = 0;
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
                    if (pressed & SCE_CTRL_CROSS) { logs_open = true; settings_row = 0; settings_error.clear(); }
                } else if (row == 20) {
                    confirm_logout = false;
                    if (pressed & SCE_CTRL_CROSS) { about_open = true; about_offset = 0; about_link = -1; about_chat_pending = false; settings_error.clear(); }
                } else if (row == 21) {
                    confirm_logout = false;
                    settings.recording_shortcuts = !settings.recording_shortcuts;
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (row == 22) {
                    confirm_logout = false;
                    settings.spoiler_reset = (settings.spoiler_reset + AppSettings::kSpoilerResetChoices + direction) % AppSettings::kSpoilerResetChoices;
                    if (settings.spoiler_reset) invalidate_spoiler_view(spoiler_visibility.reset());
                    settings_error = settings.save() ? "" : i18n::text("settings.save_failed");
                } else if (pressed & SCE_CTRL_CROSS) {
                    if (!tg.ready()) settings_error = i18n::text("settings.logout_wait");
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
                if (message_index >= 0 && message_index < (int)messages.size()) {
                    if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER)) seek_by(messages[message_index], -10000000);
                    if (pressed & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER)) seek_by(messages[message_index], 10000000);
                }
            } else if (photo_open) {
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
                    if (!count) {   // L/R rewind and fast-forward 10 s in voice and video posts
                        if (pressed & SCE_CTRL_LTRIGGER) seek_by(messages[message_index], -10000000);
                        if (pressed & SCE_CTRL_RTRIGGER) seek_by(messages[message_index], 10000000);
                    }
                    if (spoiler_locked(messages[message_index]) && messages[message_index].animation &&
                        (pressed & (SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE))) {
                        spoiler_prompt = true; spoiler_prompt_chat = tg.currentChatId();
                        spoiler_prompt_message = messages[message_index].id;
                        pressed &= ~(SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE); toggle_video = false;
                    }
                    auto lines = post_lines(font, messages[message_index]);
                    if ((pressed & SCE_CTRL_CROSS) && messages[message_index].voice) toggle_voice = true;
                    if ((pressed & SCE_CTRL_CROSS) && messages[message_index].video) toggle_video = true;
                    if ((pressed & SCE_CTRL_CROSS) && messages[message_index].document)
                        tg.downloadDocument(messages[message_index], settings.download_dir);
                    if (scrollable_post(messages[message_index])) {
                        if (pressed & SCE_CTRL_UP) post_text_scroll -= post_pitch();
                        if (pressed & SCE_CTRL_DOWN) post_text_scroll += post_pitch();
                        post_text_scroll = std::max(0, std::min(post_text_scroll, post_scroll_max(messages[message_index], lines.size())));
                    } else {
                    if ((pressed & SCE_CTRL_UP) && post_text_scroll > 0) post_text_scroll--;
                    if ((pressed & SCE_CTRL_DOWN) && post_text_scroll + 1 < (int)lines.size()) post_text_scroll++;
                    }
                    int link_line = scrollable_post(messages[message_index]) ? std::max(0, (post_text_scroll-post_text_top(messages[message_index])+62)/post_pitch()) : post_text_scroll;
                    const MessageItem& current = messages[message_index];
                    if ((pressed & SCE_CTRL_TRIANGLE) && ((current.voice && !current.audio) || current.video_note) &&
                        current.transcription.empty() && !current.transcription_pending && !current.transcription_complete) {
                        tg.requestTranscription(current.id);   // the text arrives inside the post's text panel
                    } else if (pressed & SCE_CTRL_TRIANGLE) {
                        if (messages[message_index].video && !messages[message_index].video_note) video_fullscreen = true;
                        else if (link_line < (int)lines.size() && !lines[link_line].url.empty())
                            browser_error = open_link(lines[link_line].url);
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
                if (pressed & SCE_CTRL_CIRCLE) { if (tg.inComments()) tg.closeComments(); else if (tg.inTopic()) tg.backToTopics(); else tg.leaveChat(); pressed = 0; }
                if (pressed & SCE_CTRL_SQUARE) tg.requestCompose();
                if ((pressed & SCE_CTRL_SELECT) && !(pad.buttons & SCE_CTRL_START) && tg.ready()) { upload_picker.openMenu(); chat_menu_open = false; }
                if (settings.recording_shortcuts && (pressed & SCE_CTRL_LTRIGGER)) open_circle();
                else if (settings.recording_shortcuts && (pressed & SCE_CTRL_RTRIGGER)) record_toggle = true;
                if (pressed & SCE_CTRL_TRIANGLE) tg.refreshHistory();
                if (pressed & SCE_CTRL_UP) {
                    if (message_index > 0) message_index--;
                    if (message_index <= 2) tg.loadOlder();
                }
                if ((pressed & SCE_CTRL_DOWN) && message_index + 1 < (int)messages.size()) message_index++;
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
            if (pressed & SCE_CTRL_UP) search_index--;
            if (pressed & SCE_CTRL_DOWN) search_index++;
            search_index = std::max(0, std::min(search_index, count - 1));
            if ((pressed & SCE_CTRL_CROSS) && count) {
                const ChatItem chat = tg.searchResults()[search_index];
                tg.openChat(chat.id, chat.title);
                topic_index = 0; message_index = -1; post_open = photo_open = false;
                search_open = false;
            } else if (pressed & SCE_CTRL_SQUARE) {
                search_input = true; kb.open(tr("search.kb_title"), false, tg.searchQuery());
            } else if (pressed & SCE_CTRL_TRIANGLE) tg.searchChats(tg.searchQuery());
            else if (pressed & SCE_CTRL_CIRCLE) { search_open = false; tg.clearSearch(); }
        } else if (tg.canBrowse()) {
            // The search field is the row above the first chat: Up reaches it, Down leaves it, Cross opens it.
            if (search_bar_focus) {
                if (pressed & SCE_CTRL_DOWN) search_bar_focus = false;
                if (pressed & SCE_CTRL_CROSS) pressed |= SCE_CTRL_SQUARE;
                pressed &= ~(SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_CROSS);
            } else if ((pressed & SCE_CTRL_UP) && chat_index <= 0) {
                search_bar_focus = true; pressed &= ~SCE_CTRL_UP;
            }
            if (pressed & SCE_CTRL_SQUARE) {
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
                    tg.openChat(chats[chat_index].id, chats[chat_index].title);
                    topic_index = 0;
                    message_index = -1;
                    post_open = photo_open = false;
                }
            }
            if (pressed & SCE_CTRL_TRIANGLE) tg.refreshChats();
        }

        if (record_toggle && !circle_recorder.active() && !recorder.active() && tg.currentChatId() && !kb.active() && !photo_open)
            open_audio();
        if (recorder.active() && (!tg.ready() || tg.currentChatId() != audio_chat || tg.currentTopicId() != audio_topic))
            recorder.cancel();
        recorder.pump(now);
        observe_spoiler_chat(); // Input may have left/switched the chat after pump().

        std::vector<UploadPicker::Item> upload_batch;
        if (upload_picker.takeResult(upload_batch)) {
            // Photos and videos go compressed in one album message; other files stay untouched documents.
            std::vector<std::pair<std::string, int>> files;
            for (const auto& item : upload_batch) files.emplace_back(item.path, (int)item.kind);
            if (!tg.sendLocalFiles(files, tg.currentChatId(), tg.currentTopicId()))
                browser_error = i18n::text("upload.send_failed");
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
            ProxyLink proxy; i18n::Text error;
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

        if (!settings_open && post_open && !photo_open && tg.currentChatId() && message_index>=0 && message_index<(int)tg.messages().size())
            tg.viewMessage(tg.messages()[message_index].id);
        int animation_index = !circle_recorder.active() && !recorder.active() && !settings_open && post_open && !photo_open && !kb.active() && running &&
            message_index >= 0 && message_index < (int)tg.messages().size() && tg.messages()[message_index].animation && !spoiler_locked(tg.messages()[message_index]) ? message_index : -1;
        tg.setAnimationTarget(animation_index, animation_index >= 0);
        const MessageItem* animation_message = animation_index >= 0 ? &tg.messages()[animation_index] : nullptr;
        const bool unsupported_video = animation_message && animation_message->video && !animation_message->video_note &&
            animation_message->video_width > 0 && animation_message->video_height > 0 &&
            !VideoQuality::hardwareRasterSupported(animation_message->video_width, animation_message->video_height);
        const bool prepare_video = unsupported_video && portrait_preparation_requested &&
            tg.currentChatId() == portrait_preparation_chat && animation_message->id == portrait_preparation_message;
        if (portrait_preparation_requested && !prepare_video) portrait_preparation_requested = false;
        if (unsupported_video_prompt && (!animation_message || tg.currentChatId() != unsupported_video_chat ||
            animation_message->id != unsupported_video_message)) unsupported_video_prompt = false;
        const auto* picker_focus = upload_picker.stage == UploadPicker::Stage::Review ? upload_picker.focusedItem() : nullptr;
        const bool picker_video = picker_focus && picker_focus->kind == UploadPicker::Kind::Video;
        auto animation_stream = animation_message && !unsupported_video && animation_message->animation_error.empty() &&
            animation_message->animation_stream && animation_message->animation_stream->ready() ? animation_message->animation_stream : nullptr;
        if (picker_video) animation.setSource(picker_focus->path, true, false, false);
        else animation.setSource(animation_stream ? animation_stream->source : animation_message && animation_message->animation_error.empty() ? animation_message->animation_path : "",
            animation_message && animation_message->video, animation_message && animation_message->video_note, unsupported_video, unsupported_video && !prepare_video, animation_stream);
        if (portrait_play_pending && (!animation_message || !unsupported_video ||
            tg.currentChatId() != portrait_play_chat || animation_message->id != portrait_play_message))
            portrait_play_pending = false;
        if (unsupported_video && !prepare_video && toggle_video && !animation.preparationChecked()) {
            portrait_play_pending = true;
            portrait_play_chat = tg.currentChatId(); portrait_play_message = animation_message->id;
            toggle_video = false;
        }
        if (portrait_play_pending && animation.preparationChecked()) {
            portrait_play_pending = false;
            toggle_video = true;
        }
        if (unsupported_video && !prepare_video && animation.preparationRequired() && (toggle_video || video_fullscreen)) {
            video_fullscreen = false;
            toggle_video = false;
            unsupported_video_prompt = true;
            unsupported_video_chat = tg.currentChatId();
            unsupported_video_message = animation_message->id;
        }

        if (toggle_video && (animation_message || picker_video)) animation.toggle();
        animation.upload();
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
        auto voice_stream = voice_message && voice_message->voice_error.empty() && voice_message->voice_stream &&
            voice_message->voice_stream->ready() ? voice_message->voice_stream : nullptr;
        voice.setSource(voice_stream ? voice_stream->source : voice_message && voice_message->voice_error.empty() ? voice_message->voice_path : "", false,
            voice_message && voice_message->audio, voice_stream);
        if ((toggle_voice || history_voice_toggle) && voice_message) voice.toggle();
        history_voice_toggle = false;

        // Playback needs no touches: keep the system's dim/off/suspend timers
        // alive as well as the vault idle timer. Pausing/ending restores normal idle behaviour.
        const bool video_playing = animation.playing() || (circle_playback && circle_playback->playing());
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
        bool dark = settings.isDark();
        ui_skin::set_opacity(settings.opacity);
        const unsigned int white = dark ? RGBA8(31,45,59,255) : RGBA8(255,255,255,255);
        const unsigned int ink = dark ? RGBA8(231,238,245,255) : RGBA8(39,51,66,255);
        const unsigned int muted = settings.theme == 2 ? RGBA8(194,207,237,255) : dark ? RGBA8(163,183,200,255) : RGBA8(105,122,140,255);
        const unsigned int accent = settings.theme == 2 ? RGBA8(149,214,255,255) : RGBA8(35,135,210,255);
        const unsigned int selected = dark ? RGBA8(46,72,94,255) : RGBA8(207,232,249,255);
        const unsigned int outgoing = RGBA8(20,143,82,255);
        const unsigned int separator = dark ? RGBA8(53,70,86,255) : RGBA8(205,216,226,255);
        vita2d_set_clear_color(dark ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255));
        // Textures are swapped here, between frames, only when the theme actually changed.
        // Fullscreen photo and video share one rule: the corner button hides after 1 s without presses or touches.
        const bool hide_media_ui = (photo_open || video_fullscreen) && !chat_menu_open && now - media_active_at > 1000000;
        scrub_voice.on = scrub_bar.on = scrub_play.on = scrub_ring.on = reply_hit.on = delete_hit.on = false;
        voice_play_rects.clear();   // republished by this frame's controls
        comments_rects.clear();
        reply_rects.clear();
        const bool scrub_active = touching && scrub_kind >= 1 && scrub_kind <= 3;
        struct VideoProgress { int64_t duration_us, position_us; float fraction; bool seekable, scrubbing; };
        auto video_progress = [&](const MessageItem& m) {
            VideoProgress state{(int64_t)m.video_duration * 1000000, animation.positionUs(), 0.f, false, false};
            state.seekable = animation.canSeek() && state.duration_us > 0;
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
        if (skin_theme != settings.theme || ui_skin::needs_retry()) {
            skin_theme = settings.theme;
            ui_skin::load(settings.theme);
            ui_battery::load(settings.theme);
            rich_text_skin_changed();
        }

        media_update.finish();
        performance::SlowScope draw_stage(performance::SlowPhase::Ui,"draw");
        performance::SlowScope draw_begin(performance::SlowPhase::Ui,"draw_begin");
        vita2d_start_drawing();
        vita2d_clear_screen();
        ui_skin::background();
        draw_begin.finish();
        performance::SlowScope draw_header(performance::SlowPhase::Ui,"draw_header");

        if (!ui_skin::header(36)) vita2d_draw_rectangle(0, 0, 960, 36, dark ? white : accent);
        int title_x = 20;
        if (tg.currentChatId()) {
            if (!decode_cooldown) for (size_t i=0; i<tg.chats().size(); ++i)
                if (tg.chats()[i].id == tg.currentChatId()) { tg.ensureChatMedia(i); break; }
            auto avatar = textures.get(tg.currentChatAvatar(), kMaskAvatar);
            if (tg.currentChatIsDeleted()) draw_deleted_avatar(12, 4, 28, settings.avatar_shape);
            else if (avatar) draw_texture_fit(avatar, 12, 4, 28, 28);
            else draw_avatar_placeholder(12, 4, 28, selected, settings.avatar_shape);
            title_x = 50;
        }
        const unsigned int header_text = RGBA8(240,247,255,255);
        std::string heading = settings_open ? (proxy_open ? tr("proxy.title") : logs_open ? tr("logs.title") : about_open ? tr("about.title") : encryption_open ? tr("vault.title") : tr("settings.title")) : tg.currentChatId() ? std::string(tg.currentChatTitle()) : "VitaGram " VITAGRAM_VERSION;
        std::string presence = settings_open ? "" : tg.currentUserStatus();
        // Separate title, centered presence/network status and right-aligned device status.
        constexpr float header_scale = 0.65f;
        draw_text(font, title_x, 25, fit_text(font, heading, 320-title_x, header_scale), header_text, header_scale);
        std::string network = fit_text(font, presence.empty() ? (net_ok ? net.status() : net.error()) : presence, 300, header_scale);
        draw_text(font, 480-rich_text_width(font, header_scale, network.c_str())/2, 25, network, header_text, header_scale);
        const std::string charge = battery_percent >= 0 && battery_percent <= 100 ? std::to_string(battery_percent) + "%" : "--%";
        // Lay out from the right edge: battery + percent | bold clock, with 8 px separator gaps.
        const int clock_x = 948-rich_text_width(font, header_scale, header_clock)-1;
        const int separator_x = clock_x-8-rich_text_width(font, header_scale, "|");
        const int charge_x = separator_x-8-rich_text_width(font, header_scale, charge.c_str());
        const int battery_x = charge_x-26;
        const bool battery_drawn = ui_battery::draw(battery_percent, battery_charging, battery_x, 8);
        if (!battery_drawn && battery_charging) draw_text(font, battery_x, 25, "+", header_text, header_scale);
        draw_text(font, charge_x, 25, charge, header_text, header_scale);
        draw_text(font, separator_x, 25, "|", header_text, header_scale);
        // Synthetic bold adds one pixel to the strokes without changing the font size.
        draw_text(font, clock_x, 25, header_clock, header_text, header_scale);
        draw_text(font, clock_x+1, 25, header_clock, header_text, header_scale);
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
                tr("settings.gyroscope"), tr("settings.opacity"), tr("settings.avatar_shape"), tr("settings.show_archive"), tr("settings.logs"), tr("about.title"), tr("settings.recording_shortcuts"), tr("settings.spoiler_reset"), tr("settings.logout")};
            std::string values[] = {
                language_label(),
                settings.retention == 4 ? tr("settings.always") : tr("settings.days", {std::to_string(AppSettings::days(settings.retention))}),
                settings.capacity == 5 ? tr("settings.no_limit") : tr("unit.mb", {std::to_string(AppSettings::megabytes(settings.capacity))}),
                settings.theme == 2 ? tr("settings.theme_vhbb") : dark ? tr("settings.theme_dark") : tr("settings.theme_light"), std::to_string(70+settings.font*10) + "%",
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
                    settings.spoiler_reset == 3 ? "settings.spoiler_both" : "common.disabled"), confirm_logout ? tr("settings.confirm_logout") : ""};
            {
                settings_scroll.configure(encryption_open ? kEncryptionRows : kSettingsRows, kSettingsRowHeight, kSettingsViewport);
                // Same card geometry, opacity, focus bar and scrolling as the chat list.
                vita2d_enable_clipping();
                vita2d_set_clip_rectangle(20, kSettingsTop, 940, kSettingsTop + kSettingsViewport);
                int row_y = kSettingsTop + settings_scroll.first() * kSettingsRowHeight - settings_scroll.offset();
                for (int row = settings_scroll.first(); row < settings_scroll.end(); ++row, row_y += kSettingsRowHeight) {
                    skin_card(20, row_y + 1, 920, kSettingsRowHeight - 2, row == settings_row, row == settings_row ? selected : white);
                    const int source_row = encryption_open ? kEncryptionRow + row :
                        row > kEncryptionRow ? row + kEncryptionRows - 1 : row;
                    const std::string label = !encryption_open && row == kEncryptionRow ? tr("vault.title") : labels[source_row];
                    draw_settings_text(font, settings_label_marquee, source_row, row == settings_row,
                        38, row_y + 1, 460, kSettingsRowHeight - 2, row_y + 29, label, ink, 0.65f, now);
                    draw_settings_text(font, settings_value_marquee, source_row, row == settings_row,
                        520, row_y + 1, 400, kSettingsRowHeight - 2, row_y + 29, values[source_row], ink, 0.6f, now);
                }
                vita2d_disable_clipping();
                if (settings_scroll.maximum() > 0) {
                    const float thumb = std::max(28.0f, (float)kSettingsViewport * kSettingsViewport / (kSettingsViewport + settings_scroll.maximum()));
                    const float thumb_y = kSettingsTop + (kSettingsViewport - thumb) * settings_scroll.offset() / settings_scroll.maximum();
                    if (!ui_skin::nine(ui_skin::Slice::ScrollThumb, 948, thumb_y, 8, thumb)) vita2d_draw_rectangle(953, thumb_y, 3, thumb, accent);
                }
            }
            { const Updater::Snapshot u = updater.snapshot();
              draw_text(font, 30, 500, fit_text(font, !settings_error.empty() ? settings_error : u.state == Updater::State::Failed ?
                  ((u.error == Updater::Error::Network || u.error == Updater::Error::NoRelease) ?
                      tr("update.err.github_detail", {u.detail}) : u.detail) : std::string(), 900, 0.5f), ink, 0.5f); }
            draw_text(font, 28, 528, tr("settings.footer"), ink, 0.6f);
        } else if (!net_ok) {
            draw_text(font, 30, 115, tr("net.init_failed"), ink, 0.85f);
        } else if (network_held) {
            draw_text(font, 30, 115, tr("network.held"), ink, 0.75f);
        } else if (!tg.lastError().empty() && !tg.ready()) {
            draw_text(font, 30, 115, clip(tg.lastError(), 100), ink, 0.75f);
        }

        // Settings own the content area and footer; never draw a second screen over them.
        draw_header.finish();
        performance::SlowScope draw_chat(performance::SlowPhase::Ui,"draw_chat");
        if (!settings_open && !recorder.active() && !stickers_open && !gifs_open && !tg.topicsOpen()) {
        if (tg.currentChatId() != 0) {
            const auto& msgs = tg.messages();
            if (video_fullscreen && message_index >= 0 && message_index < (int)msgs.size()) {
                vita2d_draw_rectangle(0, 0, 960, 544, ui_skin::background_color(dark ? RGBA8(16,24,33,255) : RGBA8(238,243,248,255)));
                vita2d_texture* picture = animation.texture();
                if (!picture) picture = textures.get(msgs[message_index].media_path);
                if (picture) {
                    float w = vita2d_texture_get_width(picture), h = vita2d_texture_get_height(picture);
                    float scale = std::min(960.0f/w, 544.0f/h);
                    draw_texture_fit(picture, (960-w*scale)/2, (544-h*scale)/2, 960, 544);
                }
                const MessageItem& fullscreen_message = msgs[message_index];
                const MediaLoadView load = video_load_view(fullscreen_message, animation.readyForPlayback());
                if (fullscreen_message.animation_error.empty() && (load.downloading || (load.waiting && !animation.failedToOpen()))) {
                    const float y = load.controls ? 400.f : 240.f;
                    draw_media_loading(font, 180, y, 600, load, now, accent, ink, muted);
                }
                const std::string error = !fullscreen_message.animation_error.empty() ? fullscreen_message.animation_error :
                    animation.failedToOpen() ? animation.status() : "";
                if (!error.empty()) draw_text(font, 60, 336, fit_text(font, error, 840, 0.65f), ink, 0.65f);
                if (media_ui_alpha > 0.01f && fullscreen_message.video && load.controls) {
                    const VideoProgress progress = video_progress(fullscreen_message);
                    const ControlBar bar = draw_control_bar(font, 60, 544 - 68, 840, 44, progress.position_us, progress.duration_us,
                        animation.playing(), progress.seekable, progress.scrubbing ? progress.fraction : -1.f, accent, ink, muted, media_ui_alpha);
                    if (media_ui_alpha > 0.5f) { scrub_play = bar.play; if (progress.seekable) scrub_bar = bar.track; }
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
                auto question = wrap_text(font, poll.question, 880);
                for (size_t i=0; i<question.size() && i<2; ++i) draw_text(font, 32, 102+i*26, question[i], ink, 0.7f);
                std::string kind = poll.quiz ? tr("poll.kind.quiz") : tr("poll.kind.poll");
                kind += poll.anonymous ? tr("poll.anonymous") : tr("poll.public");
                kind += poll.multiple ? tr("poll.multiple") : tr("poll.single");
                draw_text(font, 32, 158, kind, muted, 0.52f);
                int first = poll_index / 5 * 5;
                for (int row=first; row<(int)poll.order.size() && row<first+5; ++row) {
                    int index = poll.order[row]; const auto& option = poll.options[index];
                    int y = 170 + (row-first)*55;
                    skin_row(26, y, 908, 50, row==poll_index, row==poll_index ? selected : separator, true);
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
                draw_text(font, 32, 467, fit_text(font, state+tr("poll.voters", {std::to_string(poll.voters)}), 890, 0.53f), ink, 0.53f);
                if (!poll.explanation.empty()) draw_text(font, 32, 490, fit_text(font, poll.explanation, 890, 0.5f), muted, 0.5f);
            } else if (post_open && message_index >= 0 && message_index < (int)msgs.size() && scrollable_post(msgs[message_index])) {
                const auto& message = msgs[message_index];
                auto lines = post_lines(font, message);
                post_text_scroll = std::max(0, std::min(post_text_scroll, post_scroll_max(message, lines.size())));
                vita2d_enable_clipping();
                vita2d_set_clip_rectangle(0, 60, 960, 544);
                draw_post_date(font, message, post_text_scroll, selected, muted);
                int photo_y = post_photo_top-post_text_scroll;
                if (photo_y+170 > 38) {
                    for (const auto& tile : photo_carousel(message, photo_index)) {
                        if (tile.x >= 960 || tile.x+tile.width <= 0) continue;
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
                    int y = 270+kPostShift-post_text_scroll;
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
                    const int media_h = post_media_height_for(message);
                    const bool hidden_media = spoiler_locked(message);
                    const MediaLoadView load = video_load_view(message, !hidden_media && animation.readyForPlayback());
                    const bool has_video_file = !hidden_media && message.video && load.controls && message.animation_error.empty();
                    const bool media_available = message.animation_file_id > 0 || !message.animation_path.empty();
                    const bool load_card = !hidden_media && message.video && media_available && message.animation_error.empty() &&
                        (load.downloading || (load.waiting && !animation.failedToOpen() && (!unsupported_video || prepare_video)));
                    const VideoProgress video_state = video_progress(message);
                    if (media_y+media_h > 38 && media_y < 544) {
                        vita2d_texture* picture = message.animation && !hidden_media ? animation.texture() : nullptr;
                        if (!picture && !message.video_note) picture = textures.get(message.media_path, hidden_media ? kMaskBlur : kMaskNone);
                        if (!picture) picture = textures.get(message.media_fallback, hidden_media ? kMaskBlur : kMaskNone);
                        if (!picture && hidden_media) {
                            vita2d_draw_rectangle(180, media_y, 600, media_h, RGBA8(40,48,58,255));
                            draw_spoiler_particles(180, media_y, 600, media_h, now, message.id);
                        }
                        if (picture) {
                            float w = vita2d_texture_get_width(picture), h = vita2d_texture_get_height(picture);
                            float scale = std::min(900.0f/w, (float)media_h/h);
                            const float picture_x = 30+(900-w*scale)/2, picture_y = media_y+(media_h-h*scale)/2;
                            if (message.video_note) {
                                // Round clip: no frame, a progress ring around it that can be dragged to seek.
                                const float side = 316.f, note_scale = std::min(side / w, side / h);
                                draw_texture_fit(picture, 480 - w * note_scale / 2, media_y + (media_h - h * note_scale) / 2, side, side);
                                if (has_video_file) {
                                    const float cx = 480, cy = media_y + media_h / 2.f, radius = side / 2 + 11;
                                    draw_progress_ring(cx, cy, radius, 5, video_state.fraction, video_state.seekable,
                                        scale_alpha(muted, 0.5f), accent);
                                    if (video_state.seekable) scrub_ring = {true, cx, cy, radius, 0};
                                    if (!animation.playing()) {   // paused: a play mark over the picture, never covering the ring
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
                                draw_texture_fit(picture, picture_x, picture_y, 900, media_h);
                                if (hidden_media) draw_spoiler_particles(picture_x, picture_y, w*scale, h*scale, now, message.id);
                                // Video corners follow the frame's radius (frame radius 9 minus its 4 px padding).
                                if (!message.sticker) ui_skin::corners(picture_x, picture_y, w*scale, h*scale, 6);
                            }
                        }
                    }
                    if (load_card && media_y + media_h > 71 && media_y < 500) {
                        const float y = load.controls ? media_y + media_h + 48 : media_y + (media_h - 64) / 2.f;
                        draw_media_loading(font, load.controls ? 30 : 180, y, load.controls ? 900 : 600,
                            load, now, accent, ink, muted, prepare_video ? animation.status() : "");
                    }
                    std::string state = message.animation_error;
                    if (state.empty() && message.video) {
                        if (!media_available) state = tr("media.unavailable");
                        else if (unsupported_video && !prepare_video && animation.preparationRequired()) state = tr("video.unsupported_hint");
                        else if (!load_card || animation.failedToOpen()) state = animation.status();
                    } else if (state.empty() && message.animation_path.empty() && !animation_stream && message.animation) {
                        if (message.animation_file_id <= 0) state = tr("media.unavailable");
                        else if (message.animation_size > 0)
                            state = tr("media.loading", {std::to_string(std::min<int64_t>(99,
                                message.animation_downloaded * 100 / message.animation_size)) + "%"});
                        else state = tr("media.loading", {tr("unit.kib", {std::to_string(message.animation_downloaded / 1024)})});
                    } else if (state.empty() && message.animation) state =
                        message.video && !message.video_note && message.video_width > 0 && message.video_height > 0 &&
                        !VideoQuality::hardwareRasterSupported(message.video_width, message.video_height)
                        ? (animation.preparationRequired() ? tr("video.unsupported_hint") : animation.status()) : animation.status();
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
                            const ControlBar bar = draw_control_bar(font, 30, media_y+media_h+8, 900, 34, video_state.position_us, video_state.duration_us,
                                animation.playing(), video_state.seekable, video_state.scrubbing ? video_state.fraction : -1.f, accent, ink, muted);
                            scrub_play = bar.play;
                            if (video_state.seekable) scrub_bar = bar.track;
                        }
                    }
                    const int state_offset = video_bar ? (message.video_note ? 22 + scaled_px(14) : 60) : 23;
                    if (!state.empty()) draw_text(font, 30, media_y + media_h + state_offset + (load_card && load.controls && !message.video_note ? 72 : 0),
                        fit_text(font, state, 900, 0.55f), muted, 0.55f);
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
                    if (!ui_skin::nine(ui_skin::Slice::Panel, 24, panel_top, 912, panel_height, ui_skin::card_tint(false)))
                        { rounded_rect(24, panel_top, 912, panel_height, separator); rounded_rect(26, panel_top + 2, 908, panel_height - 4, white); }
                    const int header_top = panel_top + 8;
                    if (message.forwarded) draw_forward_header(font, 38, (float)header_top, 884, message, muted, accent);
                    if (message.reply_to_id) {   // same component as in the chat; tapping it opens the original
                        const float quote_top = (float)header_top + (message.forwarded ? forward_height() : 0);
                        draw_reply_quote(font, 36, quote_top, 888, message, dark, ink, accent);
                        reply_hit = {true, 36, quote_top, 888, (float)quote_height()};
                    }
                }
                for (int line=0; line<(int)lines.size(); ++line) {
                    int baseline = text_y+(int)std::lround(reveal_row(line, transcript_rows, tr_reveal) * post_pitch());
                    if (baseline < 38) continue;
                    if (baseline-22 >= 544) break;
                    const float fade = lines[line].transcript ? tr_reveal : 1.f;
                    draw_post_line(font, 38, baseline, lines[line], scale_alpha(ink, fade), scale_alpha(accent, fade), tg, textures, decode_cooldown);
                }
                vita2d_disable_clipping();
                int maximum = post_scroll_max(message, lines.size());
                if (maximum > 0) {
                    float thumb = std::max(28.0f, 482.0f*482/(482+maximum));
                    const float thumb_y = 62+(482-thumb)*post_text_scroll/maximum;
                    if (!ui_skin::nine(ui_skin::Slice::ScrollThumb, 948, thumb_y, 8, thumb))
                        vita2d_draw_rectangle(953, thumb_y, 3, thumb, accent);
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
                    auto* image = textures.get(message.photos[i].thumbnail_path, spoiler_locked(message) ? kMaskBlur : kMaskNone);
                    draw_texture_fit(image, x, y, (float)cell_w, (float)cell_h);
                    if (image && spoiler_locked(message)) {
                        const float scale = std::min(float(cell_w)/vita2d_texture_get_width(image), float(cell_h)/vita2d_texture_get_height(image));
                        draw_spoiler_particles(x, y, vita2d_texture_get_width(image)*scale, vita2d_texture_get_height(image)*scale, now, message.id+i);
                    }
                }
                int text_y = message.voice ? 346 + post_quote_extra(message) : (count == 0 ? 110 : (count > 2 ? 470 : 330));
                if (message.voice) {
                    skin_box(ui_skin::Slice::Panel, 160, 90, 640, 210, selected);
                    std::string state = message.voice_error;
                    if (state.empty() && message.voice_path.empty() && !voice_stream) {
                        if (!message.voice_file_id) state = tr("audio.unavailable");
                        else if (message.voice_size > 0) state = tr("audio.loading", {std::to_string(std::min<int64_t>(99, message.voice_downloaded * 100 / message.voice_size)) + "%"});
                        else state = tr("audio.loading", {tr("unit.kib", {std::to_string(message.voice_downloaded / 1024)})});
                    } else if (state.empty()) state = voice.status();
                    draw_text(font, 184, 130, fit_text(font, message.audio ? message.audio_title : i18n::Text(tr("audio.voice_message")), 590, 0.8f), ink, 0.8f);
                    if (message.audio) draw_text(font, 184, 157, fit_text(font, message.audio_performer, 590, 0.6f), muted, 0.6f);
                    draw_text(font, 184, 190, fit_text(font, state, 590, 0.7f), accent, 0.7f);
                    const int duration = message.voice_duration;
                    const bool voice_seekable = !message.voice_path.empty() && duration > 0;
                    const bool scrubbing_voice = scrub_active && scrub_kind == 1 && voice_seekable;
                    const double position = voice.positionSeconds();
                    float fraction = duration > 0 ? (float)std::max(0.0, std::min(1.0, position / duration)) : 0.f;
                    if (scrubbing_voice) fraction = scrub_fraction;
                    const int elapsed = scrubbing_voice ? (int)(fraction * duration) : (int)position;
                    char time[64]; std::snprintf(time, sizeof(time), "%d:%02d / %d:%02d", elapsed / 60, elapsed % 60, duration / 60, duration % 60);
                    draw_text(font, 184, 237, time, muted, 0.7f);
                    if (message.waveform.size() >= 8) draw_waveform(184, 270, 592, 38, message.waveform, fraction, accent, scale_alpha(muted, 0.55f));
                    else skin_bar(184, ui_skin::ready() ? 261 : 263, 592, ui_skin::ready() ? 10 : 6, fraction, separator, accent);
                    if (voice_seekable) scrub_voice = {true, 176, 244, 608, 54};
                }
                if (count > 6) {
                    draw_text(font, 785, 480, tr("photo.more", {std::to_string(count - 6)}), muted, 0.65f);
                }
                auto text_lines = post_lines(font, message);
                int available_lines = std::max(1, (490 - text_y) / post_pitch());
                post_text_scroll = std::max(0, std::min(post_text_scroll,
                    std::max(0, (int)text_lines.size() - available_lines)));
                const int voice_transcript_rows = count_transcript_rows(text_lines);
                if (post_has_text(message) && (!text_lines.empty() || post_quote_extra(message) > 0)) {
                    const float shown = (float)std::min(available_lines, (int)text_lines.size() - post_text_scroll) -
                        (post_text_scroll == 0 ? voice_transcript_rows * (1.f - tr_reveal) : 0.f);
                    const int frame_top = text_y - post_quote_extra(message) - 10 - scaled_px(18);
                    const int frame_height = post_quote_extra(message) + (text_lines.empty() ? 6 :
                        10 + scaled_px(18) + (int)std::lround(std::max(0.f, shown - 1) * post_pitch()) + kPanelBottomPad);
                    if (!ui_skin::nine(ui_skin::Slice::Panel, 24, frame_top, 912, frame_height, ui_skin::card_tint(false)))
                        { rounded_rect(24, frame_top, 912, frame_height, separator); rounded_rect(26, frame_top + 2, 908, frame_height - 4, white); }
                    if (message.forwarded) draw_forward_header(font, 38, (float)frame_top + 8, 884, message, muted, accent);
                    if (message.reply_to_id) {
                        const float quote_top = (float)frame_top + 8 + (message.forwarded ? forward_height() : 0);
                        draw_reply_quote(font, 36, quote_top, 888, message, dark, ink, accent);
                        reply_hit = {true, 36, quote_top, 888, (float)quote_height()};
                    }
                }
                for (int line = 0; line < available_lines && post_text_scroll + line < (int)text_lines.size(); ++line) {
                    const bool revealing = post_text_scroll == 0 && tr_reveal < 1.f;
                    const float row = revealing ? reveal_row(line, voice_transcript_rows, tr_reveal) : (float)line;
                    const float fade = revealing && text_lines[post_text_scroll + line].transcript ? tr_reveal : 1.f;
                    const unsigned int base = message.outgoing ? outgoing : ink;
                    draw_post_line(font, 38, text_y + (int)std::lround(row * post_pitch()), text_lines[post_text_scroll + line],
                        scale_alpha(base, fade), scale_alpha(accent, fade), tg, textures, decode_cooldown);
                }
            } else {
                auto history_rows = history_layout(font, msgs, message_index, tg.currentChatIsGroup());
                vita2d_enable_clipping();
                vita2d_set_clip_rectangle(0, 60, 960, 544);
                for (const auto& entry : history_rows) {
                    int i = entry.index;
                    if (!decode_cooldown) tg.ensureMessageMedia((size_t)i);
                    if (entry.day_top >= 0) {
                        int width = rich_text_width(font, 0.55f, entry.day.c_str()) + 24;
                        skin_box(ui_skin::Slice::DateChip, 480-width/2, entry.day_top, width, 22, selected, true);
                        draw_text(font, 492-width/2, entry.day_top+16, entry.day, muted, 0.55f);
                    }
                    int y = entry.top + 4;
                    const MessageItem& m = msgs[i];
                    if (m.service_message) {
                        std::string label = fit_text(font, tg.serviceMessageText(m), 860, 0.55f);
                        int width = rich_text_width(font, 0.55f, label.c_str()) + 28;
                        int left = 480-width/2;
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
                    bool show_sender = tg.currentChatIsGroup() && !m.outgoing;
                    std::string sender = show_sender ? fit_text(font, tg.senderName(m), 654, 0.55f) : "";
                    bool selected_message = i == message_index;
                    std::string preview_path = m.document ? "" : !m.photos.empty() ? m.photos.front().thumbnail_path : m.media_path;
                    const bool spoiler_hidden = spoiler_locked(m);
                    vita2d_texture* media = textures.get(preview_path, spoiler_hidden ? kMaskBlur : m.video_note ? kMaskRoundThumb : kMaskNone);
                    if (!media) media = textures.get(m.media_fallback, spoiler_hidden ? kMaskBlur : kMaskNone);
                    bool has_media = !m.photos.empty() || m.media_file_id > 0 || m.animation;
                    int media_width = has_media ? 108 : 0;
                    int max_text_width = 680 - media_width;
                    auto lines = wrap_text(font, clip(m.text, 2048), max_text_width);
                    if (lines.size() > 2) {
                        lines.resize(2);
                        lines.back() = fit_text(font, lines.back() + "...", max_text_width, 0.68f);
                    }
                    int text_width = m.photos.size() > 1 ? 230 : (m.voice && !m.audio) ? 400 : 130;
                    for (const auto& line : lines) text_width = std::max(text_width, rich_text_width(font, 0.68f, line.c_str()));
                    text_width = std::max(text_width, rich_text_width(font, 0.52f, m.media_label.c_str()) + (m.document ? 17 : 0));
                    int bubble_width = std::min(712, text_width + media_width + 32);
                    if (m.has_comments)   // room for the badge left of the timestamp
                        bubble_width = std::max(bubble_width, std::min(712, comments_badge_width(font, m) + rich_text_width(font, 0.52f, message_time(m).c_str()) + (m.outgoing ? 46 : 16) + 40));
                    if (show_sender) bubble_width = std::max(bubble_width, rich_text_width(font, 0.55f, sender.c_str()) + 58);
                    if (m.forwarded) {
                        const int header_width = rich_text_width(font, 0.45f, tr("forward.from").c_str()) + rich_text_width(font, 0.5f, (m.forward_source.empty() ? tr("forward.unknown") : m.forward_source).c_str()) + 44;
                        bubble_width = std::max(bubble_width, std::min(480, header_width));
                    }
                    if (m.reply_to_id) {   // the quote panel gets a readable width, never wider than the card allows
                        const int quote_width = std::max(rich_text_width(font, 0.5f, m.reply_sender.c_str()), rich_text_width(font, 0.55f, m.reply_text.c_str())) + 54;
                        bubble_width = std::max(bubble_width, std::min(480, quote_width));
                    }
                    int bubble_x = m.outgoing ? 928 - bubble_width : 32;
                    unsigned int bubble_color = m.outgoing ? (dark ? RGBA8(43,82,112,255) : RGBA8(211,242,207,255)) : white;
                    // The selected card uses the focused PNG, which carries its own outline.
                    const unsigned int outgoing_tint = dark ? RGBA8(170,235,200,255) : RGBA8(205,245,215,255);
                    const bool skinned_card = ui_skin::nine(selected_message ? ui_skin::Slice::RowFocused : ui_skin::Slice::Panel,
                        bubble_x, y-4, bubble_width, entry.height-6, ui_skin::card_tint(selected_message, m.outgoing && !selected_message ? outgoing_tint : 0x00FFFFFFu));
                    if (!skinned_card) {
                        if (selected_message) rounded_rect(bubble_x-2, y-6, bubble_width+4, entry.height-2, accent);
                        rounded_rect(bubble_x, y-4, bubble_width, entry.height-6, bubble_color);
                    }
                    const int grow = history_pitch() - 22;
                    y += kCardPad;   // breathing room between the card border and avatar/name/text
                    if (show_sender) {
                        static const unsigned int colors[] = {
                            static_cast<unsigned int>(RGBA8(25,115,156,255)),
                            static_cast<unsigned int>(RGBA8(135,72,162,255)),
                            static_cast<unsigned int>(RGBA8(36,128,93,255)),
                            static_cast<unsigned int>(RGBA8(167,86,37,255))
                        };
                        unsigned int sender_color = colors[static_cast<uint64_t>(m.sender_id)%4];
                        if (dark) sender_color = (sender_color & 0xFF000000u) | ((sender_color & 0x00FEFEFEu) >> 1) | 0x00808080u;
                        vita2d_texture* sender_avatar = textures.get(tg.senderAvatar(m), kMaskAvatar);
                        if (tg.senderIsDeleted(m)) draw_deleted_avatar(bubble_x+14, y-3, 18, settings.avatar_shape);
                        else if (sender_avatar) draw_texture_fit(sender_avatar, bubble_x+14, y-3, 18, 18);
                        else draw_avatar_placeholder(bubble_x+14, y-3, 18, sender_color, settings.avatar_shape);
                        draw_text(font, bubble_x+40, y+13, sender, sender_color, 0.55f);
                        y += std::max(20, scaled_px(20));
                    }
                    if (m.forwarded) {   // origin first, then the reply quote and the text
                        draw_forward_header(font, (float)bubble_x + 14, (float)y - 2, (float)bubble_width - 28, m, muted, accent);
                        y += forward_height();
                    }
                    if (m.reply_to_id) {   // between the name and the new text, spaced like the rest of the card
                        draw_reply_quote(font, (float)bubble_x + 10, (float)y - 2, (float)bubble_width - 20, m, dark, ink, accent);
                        reply_rects.push_back({m.id, {true, (float)bubble_x + 10, (float)y - 2, (float)bubble_width - 20, (float)quote_height()}});
                        y += quote_height() + 4;
                    }
                    // Small tail points toward the sender's side.
                    const int label_row = message_body_end(m, lines.size()), time_row = message_time_row(m, lines.size());
                    if (!skinned_card) vita2d_draw_rectangle(m.outgoing ? bubble_x+bubble_width-8 : bubble_x, y+time_row-7, 8, 14, bubble_color);
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
                        // Play/pause button from the icon atlas and the waveform; the button starts playback in place.
                        const bool current = voice_message && voice_message->id == m.id;
                        const bool playing = current && voice.playing();
                        const float disc_x = (float)bubble_x + 36, disc_y = (float)y + 33;
                        vita2d_draw_fill_circle(disc_x, disc_y, 23, accent);
                        if (!ui_skin::icon(playing ? ui_skin::Icon::Pause : ui_skin::Icon::Play, disc_x - 15, disc_y - 15, 30, RGBA8(255, 255, 255, 255))) {
                            if (playing) { vita2d_draw_rectangle(disc_x - 7, disc_y - 8, 5, 16, RGBA8(255,255,255,255)); vita2d_draw_rectangle(disc_x + 2, disc_y - 8, 5, 16, RGBA8(255,255,255,255)); }
                            else vita2d_draw_rectangle(disc_x - 4, disc_y - 8, 12, 16, RGBA8(255,255,255,255));
                        }
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
                    for (size_t row = 0; row < lines.size(); ++row)
                        draw_text(font, text_x, y+20+grow+(int)row*history_pitch(), lines[row], ink, 0.68f);
                    if (!m.media_label.empty()) {
                        int label_x = text_x;
                        if (m.document) {
                            if (!ui_skin::icon(ui_skin::Icon::File, label_x-3, y+label_row-16, 16)) {
                                vita2d_draw_rectangle(label_x, y+label_row-13, 10, 12, muted);
                                vita2d_draw_rectangle(label_x+7, y+label_row-13, 3, 3, bubble_color);
                            }
                            label_x += 15;
                        }
                        draw_text(font, label_x, y + label_row, m.media_label, muted, 0.52f);
                    }
                    std::string timestamp = message_time(m);
                    int time_right = bubble_x + bubble_width - (m.outgoing ? 46 : 16);
                    draw_text(font, time_right - rich_text_width(font, 0.52f, timestamp.c_str()), y+time_row, timestamp, muted, 0.52f);
                    if (m.outgoing) draw_delivery((float)(bubble_x+bubble_width-41), (float)y+time_row-11,
                                                  m.delivery, skinned_card ? ui_skin::surface_color(bubble_color) : bubble_color, now);
                    if (m.has_comments) {
                        const int badge_right = time_right - rich_text_width(font, 0.52f, timestamp.c_str()) - 16;
                        const int badge_left = draw_comments_badge(font, badge_right, y + time_row, m, ink, accent);
                        comments_rects.push_back({m.id, {true, (float)badge_left - 10, (float)(y + time_row - 18), (float)(badge_right - badge_left + 20), 26}});
                    }
                }
                vita2d_disable_clipping();
                if (msgs.empty()) draw_text(font, 38, 150, tr("chat.no_messages"), muted, 0.75f);
                draw_attach_button(dark);
            }
        } else if (search_open && tg.canBrowse()) {
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
                else draw_avatar_placeholder(32, y + 4, 32, RGBA8(126,176,211,255), settings.avatar_shape);
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
        } else if (tg.canBrowse()) {
            const auto& chats = tg.chats();
            search_bar(font, tr("search.title"), search_bar_focus ? ink : muted, search_bar_focus ? selected : white, search_bar_focus);
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
                vita2d_set_clip_rectangle(20, 69 + kSearchBarShift, 940, 113 + kSearchBarShift);
                for (int i = 0; i < (int)tg.folders().size(); ++i) {
                    int x = 20 + i * 180 - folder_scroll;
                    if (x + 176 <= 20 || x >= 940) continue;
                    const bool tab_skinned = ui_skin::nine(i == active ? ui_skin::Slice::TabActive : ui_skin::Slice::TabNormal, x, 71 + kSearchBarShift, 176, 39);
                    if (!tab_skinned) vita2d_draw_rectangle(x, 71 + kSearchBarShift, 176, 39, i == active ? selected : white);
                    draw_text(font, x+10, 96 + kSearchBarShift, fit_text(font, tg.folders()[i].title, 156, 0.65f), i == active && !tab_skinned ? accent : ink, 0.65f);
                    if (i == active && !(tab_skinned && ui_skin::indicator(x+24, 105 + kSearchBarShift, 128, 4))) vita2d_draw_rectangle(x, 108 + kSearchBarShift, 176, 3, accent);
                }
                vita2d_disable_clipping();
                if (folder_scroll > 0 && !ui_skin::icon(ui_skin::Icon::Back, -4, 79 + kSearchBarShift, 24)) draw_text(font, 2, 97 + kSearchBarShift, "<", accent, 0.6f);
                if (folder_scroll < folder_max_scroll && !ui_skin::icon(ui_skin::Icon::Next, 940, 79 + kSearchBarShift, 24)) draw_text(font, 944, 97 + kSearchBarShift, ">", accent, 0.6f);
            } else { shown_folder_id = -1; shown_folder_index = -1; }
            chat_scroll.configure((int)chats.size(), chat_row_height, 544-chat_top);
            const int base = chat_scroll.first(), end = chat_scroll.end();
            int y = chat_top + 24 + base * chat_row_height - chat_scroll.offset();
            if (!decode_cooldown) for (int i = base; i < end; ++i) tg.ensureChatMedia((size_t)i);
            vita2d_enable_clipping();
            vita2d_set_clip_rectangle(20, chat_top, 940, 544);
            const int focus_row = search_bar_focus ? -1 : chat_index;
            for (int i = base; i < end; ++i) {
                if (!ui_skin::nine(i == focus_row ? ui_skin::Slice::RowFocused : ui_skin::Slice::Row, 20, y - 23, 920, chat_row_height - 2, ui_skin::card_tint(i == focus_row))) {
                    vita2d_draw_rectangle(20, y - 24, 920, chat_row_height, i == focus_row ? selected : white);
                    vita2d_draw_rectangle(88, y - 25 + chat_row_height, 852, 1, separator);
                }
                vita2d_texture* avatar = textures.get(chats[i].avatar_path, kMaskAvatar);
                // The avatar and title are centred in the row, whatever height the layout gives it.
                const float row_top = (float)(y - 23), row_inner = (float)(chat_row_height - 2);
                const float avatar_size = std::min(42.f, row_inner - 6.f), avatar_x = 34 + (42.f - avatar_size) / 2, avatar_y = row_top + (row_inner - avatar_size) / 2;
                if (tg.chatIsDeleted(chats[i])) draw_deleted_avatar(avatar_x, avatar_y, avatar_size, settings.avatar_shape);
                else if (avatar) draw_texture_fit(avatar, avatar_x, avatar_y, avatar_size, avatar_size);
                else draw_avatar_placeholder(avatar_x, avatar_y, avatar_size, RGBA8(126,176,211,255), settings.avatar_shape);
                draw_text(font, 88, (int)(row_top + row_inner / 2) + 7, clip(tg.chatTitle(chats[i]), 70), ink, 0.72f);
                if (chats[i].unread_count > 0 || chats[i].marked_unread) {
                    // Equidistant from the top, bottom and right edges of the row.
                    const float radius = 6.0f, row_h = (float)(chat_row_height - 2);
                    const float cx = 940.0f - row_h / 2.0f, cy = (float)(y - 23) + row_h / 2.0f;
                    for (int g = 4; g >= 1; --g)
                        vita2d_draw_fill_circle(cx, cy, radius + g * 2.0f, RGBA8(30,144,255,(unsigned)(14 + (5 - g) * 4)));
                    vita2d_draw_fill_circle(cx, cy, radius, RGBA8(30,144,255,255));
                }
                y += chat_row_height;
            }
            vita2d_disable_clipping();
            if (chats.empty()) draw_text(font, 38, chat_top+51, tg.chatsLoading() ? tr("chats.loading_first") :
                (tg.chatsEnd() ? tr("chats.none") : tr("chats.load_failed")), muted, 0.75f);
            skin_strip(selected);
            const std::string progress = tr("chats.status", {tg.chatsLoading() ? tr("chats.state_loading") : tr("chats.state_ready"),
                std::to_string(chats.size()), tg.chatsEnd() ? tr("chats.all_loaded") : tr("chats.tap_more")});
            const Updater::Snapshot update_state = updater.snapshot();
            const std::string update_hint = update_state.state == Updater::State::Available ? tr("update.hint", {update_state.version}) + "   " : "";
            draw_text(font, 30, 58, !update_notice.empty() ? fit_text(font, update_notice, 900, 0.48f) :
                fit_text(font, update_hint + (tg.chatsError().empty() ? progress : tr("chats.retry", {tg.chatsError()})), 900, 0.48f), ink, 0.48f);
            if (tg.chatsLoading()) {
                float position = (float)((now / 2000) % 840);
                if (!ui_skin::indicator(position + 20, 61, 120, 3)) vita2d_draw_rectangle(position + 20, 62, 120, 2, accent);
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
        if (!settings_open && reactions_open) {
            skin_backdrop(36, 508, white);
            draw_text(font, 32, 80, tr("reaction.title"), ink, 0.85f);
            draw_text(font, 32, 112, fit_text(font, tg.reactionStatus(), 880, 0.6f), muted, 0.6f);
            const auto& choices = tg.reactionChoices();
            int begin = reaction_index/32*32;
            for (int i=begin; i<(int)choices.size() && i<begin+32; ++i) {
                int x=32+(i-begin)%8*112, y=130+(i-begin)/8*72;
                skin_row(x, y, 104, 64, i == reaction_index, i == reaction_index ? selected : separator, true);
                if (!decode_cooldown) tg.ensureReactionMedia(i);
                auto icon=textures.get(tg.reactionIcon(i));
                if (icon) draw_texture_fit(icon,x+28,y+8,48,48);
                else draw_text(font, x+8, y+43, fit_text(font,choices[i],88,0.7f), ink, 0.7f);
            }
        }
        if (!settings_open && tg.topicsOpen()) {
            skin_backdrop(36, 508, white);
            draw_text(font, 30, 61, tr("topics.title"), ink, 0.7f);
            const auto& topics = tg.topics();
            int first = std::max(0, topic_index-7);
            for (int i = first; i < std::min(first+8, (int)topics.size()); ++i) {
                int y = 70+(i-first)*50;
                skin_row(20, y, 920, 48, i == topic_index, i == topic_index ? selected : white);
                std::string title = topics[i].title + (topics[i].closed ? tr("topics.closed") : "");
                draw_text(font, 34, y+32, fit_text(font, title, 740, 0.75f), ink, 0.75f);
                if (topics[i].unread) draw_text(font, 830, y+32, std::to_string(topics[i].unread), accent, 0.7f);
            }
            draw_text(font, 30, 492, fit_text(font, tg.topicsStatus(), 900, 0.55f), muted, 0.55f);
        }
        if (!settings_open && stickers_open) {
            skin_backdrop(36, 508, white);
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
                std::string title = sticker_pack < (int)packs.size() ? packs[sticker_pack].title : i18n::Text(tr("sticker.title"));
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
                draw_text(font, 30, 490, tr("sticker.page", {std::to_string(first/18+1), std::to_string(std::max(1, ((int)items.size()+17)/18))}), muted, 0.6f);
            }
        }
        if (!settings_open && gifs_open) {
            skin_backdrop(36, 508, white);
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
        draw_overlays.finish();
        if (recorder.active()) {
            const auto state = recorder.state();
            skin_backdrop(0, 544, white);
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
            skin_backdrop(0, 544, white);
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
        if (chat_ui_visible()) {
            float t = chat_menu_open ? std::min(1.f, (float)(now - chat_menu_since) / 140000.f) : 0.f;
            const float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
            if (chat_menu_open) {
                const auto& items = chat_menu_snapshot;
                const int first = menu_first((int)items.size(), chat_menu_sel);
                const int visible = std::min(kMenuMaxRows, (int)items.size());
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
                    const std::string label = tr(items[i].label);
                    const bool with_icon = items[i].icon >= 0 &&
                        ui_skin::icon((ui_skin::Icon)items[i].icon, kMenuX + 16 + dx, rows_top + (i - first) * kMenuRowHeight + 7, 22, fade);
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
        else if (kb.active() && !settings_open && !qr_scanner.active() && (tg.currentChatId() != 0 || tg.topicsOpen() || tg.canBrowse()) &&
                 upload_picker.stage == UploadPicker::Stage::Closed)
            draw_menu_button(dark);   // stays visible under the system keyboard, like the attach button
        {
            static UploadPicker::Stage previous_stage = UploadPicker::Stage::Closed;
            if (upload_picker.stage == UploadPicker::Stage::Menu && previous_stage != UploadPicker::Stage::Menu) attach_menu_since = now;
            previous_stage = upload_picker.stage;
        }
        if (chat_ui_visible() && tg.currentChatId() && post_open && !photo_open && !circle_recorder.active() && !recorder.active())
            draw_attach_button(dark);
        if (chat_ui_visible() && tg.ready() && post_open && !photo_open && !video_fullscreen &&
            !stickers_open && !gifs_open && !reactions_open && !upload_picker.active() && !chat_menu_open && !chat_menu_waiting &&
            !circle_recorder.active() && !recorder.active() && message_index >= 0 && message_index < (int)tg.messages().size() &&
            tg.canDeleteMessage(tg.messages()[message_index].id)) {
            skin_box(ui_skin::Slice::DateChip, 766, 68, 168, 28, selected, true);
            ui_skin::icon(ui_skin::Icon::Trash, 773, 72, 20);
            draw_text(font, 799, 87, fit_text(font, tr("menu.delete_message"), 128, 0.46f), ink, 0.46f);
            delete_hit = {true, 766, 68, 168, 28};
            delete_hit_message = tg.messages()[message_index].id;
        }
        if (upload_picker.active()) {
            using Stage = UploadPicker::Stage;
            const char* kind_key[] = {"upload.image", "upload.video", "upload.file", "upload.video_note", "upload.voice_message"};
            if (upload_picker.stage == Stage::Menu) {
                // Compact glass popup growing out of the attach button (~140 ms ease-out).
                float t = std::min(1.f, (float)(now - attach_menu_since) / 140000.f);
                const float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
                const float dy = (1.f - e) * 30.f, dx = -(1.f - e) * 36.f; // grows out of the circle
                const unsigned int fade = RGBA8(255, 255, 255, (unsigned int)(255.f * e));
                const auto items = attach_menu_items();
                const int panel_height = (int)items.size() * attach_menu_row + 2 * kMenuPad;
                const int panel_top = attach_menu_bottom - panel_height, top = panel_top + kMenuPad;
                upload_picker.selected = std::max(0, std::min(upload_picker.selected, (int)items.size()-1));
                const bool menu_skin = ui_skin::nine(ui_skin::Slice::Menu, 36 + dx, panel_top - 4 + dy, 308, panel_height + 8, fade);
                if (!menu_skin) {
                    glass_panel(40 + dx, panel_top + dy, 300, panel_height, scale_alpha(RGBA8(52, 58, 68, 232), e), scale_alpha(RGBA8(20, 25, 33, 238), e));
                    vita2d_draw_rectangle(50 + dx, panel_top + dy, 280, 1, scale_alpha(RGBA8(255, 255, 255, 90), e));
                }
                for (int i = 0; i < (int)items.size(); ++i) {
                    if (i == upload_picker.selected && !(menu_skin && ui_skin::nine(ui_skin::Slice::MenuItemFocus, 48 + dx, top + 2 + i * 40 + dy, 284, 36, fade)))
                        glass_panel(48 + dx, top + 2 + i * 40 + dy, 284, 36, scale_alpha(RGBA8(255, 255, 255, 62), e), scale_alpha(RGBA8(255, 255, 255, 26), e), 7);
                    if (i && !menu_skin) vita2d_draw_rectangle(48 + dx, top + i * 40 + dy, 284, 1, scale_alpha(RGBA8(255, 255, 255, 36), e));
                    const bool icon = items[i].icon >= 0 && ui_skin::icon((ui_skin::Icon)items[i].icon,
                        54 + dx, top + 10 + i * 40 + dy, 22, fade);
                    draw_text(font, 60 + dx + (icon ? 28 : 0), top + 30 + i * 40 + dy,
                        fit_text(font, tr(items[i].label), icon ? 246 : 274, 0.62f),
                        scale_alpha(menu_skin ? ink : RGBA8(240, 244, 249, 255), e), 0.62f);
                }
                draw_attach_button(dark, e);
            } else if (upload_picker.stage == Stage::Browse) {
                skin_backdrop(36, 508, white);
                const bool preview = upload_picker.kind == UploadPicker::Kind::Photo;
                const float list_w = preview ? 496.f : 896.f;   // same split as the review form
                draw_text(font, 30, 68, fit_text(font, upload_picker.path.empty() ? tr("picker.drives") : upload_picker.path == UploadPicker::LatestView ? tr("upload.latest") : upload_picker.path, 900, 0.64f), ink, 0.64f);
                draw_text(font, 32, 97, fit_text(font, tr(kind_key[(int)upload_picker.kind]) + "  |  " +
                    tr(upload_picker.show_hidden ? "upload.hidden_on" : "upload.hidden_off"), 900, 0.48f), muted, 0.48f);
                skin_frame(24, 109, list_w + 16, 356, separator, white);
                const int first = (upload_picker.selected / 8) * 8;
                for (int row = 0; row < 8 && first + row < (int)upload_picker.entries.size(); ++row) {
                    const auto& entry = upload_picker.entries[first + row];
                    if (first + row == upload_picker.selected) skin_row(32, 117 + row * 43, list_w, 40, true, selected, true);
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
                        draw_texture_fit(textures.get(full), 60 + list_w, 117, 868 - list_w, 340);
                    }
                }
                if (upload_picker.entries.empty() && upload_picker.error.empty())
                    draw_text(font, 42, 144, fit_text(font, upload_picker.info.empty() ? tr("upload.empty") : upload_picker.info, 520, 0.5f), muted, 0.5f);
                if (!upload_picker.error.empty()) draw_text(font, 30, 484, upload_picker.error, ink, 0.55f);
                draw_text(font, 28, 528, tr("upload.browse_footer"), ink, 0.6f);
            } else {
                skin_backdrop(36, 508, white);
                const int count = (int)upload_picker.items.size();
                draw_text(font, 30, 76, tr("upload.review_title", {std::to_string(count), std::to_string(UploadPicker::MaxItems)}), ink, 0.8f);
                skin_frame(24, 109, 512, 356, separator, white);
                const int first = (upload_picker.selected / 8) * 8;
                const char* add_key[] = {"upload.add_image", "upload.add_video", "upload.add_file"};
                for (int row = 0; row < 8 && first + row < upload_picker.reviewRows(); ++row) {
                    const int index = first + row;
                    if (index == upload_picker.selected) skin_row(32, 117 + row * 43, 496, 40, true, selected, true);
                    std::string label;
                    if (index < count) label = upload_picker.items[index].name;
                    else if (index < count + 3) label = tr(add_key[index - count]);
                    else label = tr("upload.send", {std::to_string(count)});
                    draw_text(font, 42, 144 + row * 43, fit_text(font, label, index < count ? 400 : 470, 0.65f),
                              index == count + 3 && count == 0 ? muted : ink, 0.65f);
                    if (index < count) draw_text(font, 450, 143 + row * 43, UploadPicker::formatSize(upload_picker.items[index].size), muted, 0.45f);
                }
                skin_frame(548, 109, 388, 356, separator, white);
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
        if (kb.active()) kb.draw(font);
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
        if (!settings_open && !stickers_open && !gifs_open && !reactions_open && !tg.topicsOpen() && !circle_recorder.active() && !recorder.active() && !photo_open && !kb.active())
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
    recorder.cancel();
    animation.shutdown();
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
