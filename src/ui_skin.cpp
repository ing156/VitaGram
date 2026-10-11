#include "ui_skin.h"
#include <jansson.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

namespace ui_skin {
namespace {

constexpr const char* kRoot = "app0:ui/";
#ifdef VITAGRAM_UI_ALT_BACKGROUND
constexpr const char* kBackgroundFile = "background_alt_960x544.png";
#else
constexpr const char* kBackgroundFile = "background_960x544.png";
#endif

constexpr std::array<const char*, (size_t)Slice::Count> kSliceNames = {
    "panel", "row", "row_focused", "tab_normal", "tab_active", "menu", "menu_item_focus", "date_chip",
    "media_frame", "focus_ring", "progress_track", "progress_fill", "scroll_thumb", "dialog"};
constexpr std::array<const char*, (size_t)Icon::Count> kIconNames = {"file", "back", "next", "play", "pause", "search", "check", "close"};
constexpr std::array<const char*, 9> kButtonNames = {"triangle", "square", "cross", "circle", "dpad", "START", "SELECT", "L", "R"};

struct SliceTexture {
    vita2d_texture* texture = nullptr;
    float width = 0, height = 0;
    float left = 0, top = 0, right = 0, bottom = 0;
};

struct State {
    bool ready = false;
    bool dark = false;
    int attempted = -1;      // theme of the last load() attempt, successful or not
    int retries = 0;         // reloads of the same theme after an incomplete attempt
    unsigned long long retry_at = 0;   // process time (us) before which no retry is made
    int opacity = 75;
    std::array<SliceTexture, (size_t)Slice::Count> slices;
    std::array<vita2d_texture*, (size_t)Image::Count> images{};
    vita2d_texture* corner_buttons = nullptr;   // theme independent; kept until unload()
    vita2d_texture* corner = nullptr;   // 16x16: opaque outside a radius-8 rounded square, transparent inside
    vita2d_texture* icons = nullptr;
    vita2d_texture* buttons = nullptr;
    std::array<Sprite, (size_t)Icon::Count> icon_sprites;
    std::array<Sprite, 9> button_sprites;
    unsigned int background_color = 0, surface_color = 0;
    bool colors = false;
};

State g;

// Reads a whole file with the raw sceIo API. The newlib descriptor table is shared with worker threads and
// an app0: read through it was seen returning stray bytes or nothing at startup; a short read is retried.
bool read_file(const std::string& path, std::vector<unsigned char>& out) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt) sceKernelDelayThread(50 * 1000);
        SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
        if (fd < 0) continue;
        const SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
        sceIoLseek(fd, 0, SCE_SEEK_SET);
        bool complete = size > 0 && size < (32 << 20);
        if (complete) {
            out.assign((size_t)size + 1, 0);   // trailing zero keeps the buffer a valid C string
            size_t done = 0;
            while (done < (size_t)size) {
                const int n = sceIoRead(fd, out.data() + done, (size_t)size - done);
                if (n <= 0) break;
                done += (size_t)n;
            }
            complete = done == (size_t)size;
            if (complete) out.resize((size_t)size);
        }
        sceIoClose(fd);
        if (complete) return true;
    }
    return false;
}

vita2d_texture* load_png(const std::string& relative, bool linear) {
    const std::string path = std::string(kRoot) + relative;
    std::vector<unsigned char> bytes;
    vita2d_texture* texture = read_file(path, bytes) ? vita2d_load_PNG_buffer(bytes.data()) : nullptr;
    if (!texture) { std::fprintf(stderr, "ui_skin: cannot load %s\n", path.c_str()); return nullptr; }
    const auto filter = linear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
    vita2d_texture_set_filters(texture, filter, filter);
    return texture;
}

bool json_int(json_t* value, int& out) {
    if (!json_is_number(value)) return false;
    out = (int)json_number_value(value);
    return true;
}

bool parse_color(json_t* value, unsigned int& out) {
    if (!json_is_string(value)) return false;
    const char* text = json_string_value(value);
    if (!text || text[0] != '#' || std::string(text).size() != 7) return false;
    const unsigned long rgb = std::strtoul(text + 1, nullptr, 16);
    out = RGBA8((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255);
    return true;
}

bool read_rect(json_t* object, Sprite& sprite) {
    int x, y, w, h;
    if (!json_is_object(object) || !json_int(json_object_get(object, "x"), x) || !json_int(json_object_get(object, "y"), y) ||
        !json_int(json_object_get(object, "w"), w) || !json_int(json_object_get(object, "h"), h) || w <= 0 || h <= 0) return false;
    sprite.x = (float)x; sprite.y = (float)y; sprite.w = (float)w; sprite.h = (float)h;
    sprite.vx = 0; sprite.vy = 0; sprite.vw = (float)w; sprite.vh = (float)h;
    return true;
}

// Bounds of the pixels that are actually drawn: button sprites sit in a transparent 48x48 cell.
void measure_visible(const vita2d_texture* texture, Sprite& sprite) {
    const auto* pixels = static_cast<const unsigned char*>(vita2d_texture_get_datap(texture));
    const size_t stride = vita2d_texture_get_stride(texture);
    int min_x = (int)sprite.w, min_y = (int)sprite.h, max_x = -1, max_y = -1;
    for (int y = 0; y < (int)sprite.h; ++y) {
        const unsigned char* row = pixels + (size_t)((int)sprite.y + y) * stride + (size_t)(int)sprite.x * 4;
        for (int x = 0; x < (int)sprite.w; ++x) {
            if (row[x * 4 + 3] <= 20) continue;
            min_x = std::min(min_x, x); max_x = std::max(max_x, x);
            min_y = std::min(min_y, y); max_y = std::max(max_y, y);
        }
    }
    if (max_x < min_x || max_y < min_y) return;
    sprite.vx = (float)min_x; sprite.vy = (float)min_y;
    sprite.vw = (float)(max_x - min_x + 1); sprite.vh = (float)(max_y - min_y + 1);
}

void free_all() {
    for (auto& slice : g.slices) { if (slice.texture) vita2d_free_texture(slice.texture); slice = {}; }
    for (auto& image : g.images) { if (image) vita2d_free_texture(image); image = nullptr; }
    if (g.corner) vita2d_free_texture(g.corner);
    g.corner = nullptr;
    if (g.icons) vita2d_free_texture(g.icons);
    if (g.buttons) vita2d_free_texture(g.buttons);
    g.icons = g.buttons = nullptr;
    for (auto& sprite : g.icon_sprites) sprite = {};
    for (auto& sprite : g.button_sprites) sprite = {};
    g.colors = false;
    g.ready = false;
}

// Loads one atlas and resolves its named sprites from the manifest.
template <size_t N>
vita2d_texture* load_atlas(json_t* root, const std::string& relative, const std::array<const char*, N>& names,
                           std::array<Sprite, N>& sprites, bool measure) {
    json_t* atlas = json_object_get(json_object_get(root, "sprite_atlases"), relative.c_str());
    json_t* entries = json_object_get(atlas, "sprites");
    if (!json_is_object(entries)) return nullptr;
    vita2d_texture* texture = load_png(relative, true);
    if (!texture) return nullptr;
    int atlas_size[2] = {0, 0};
    json_t* size = json_object_get(atlas, "size");
    if (!json_is_array(size) || !json_int(json_array_get(size, 0), atlas_size[0]) || !json_int(json_array_get(size, 1), atlas_size[1]) ||
        atlas_size[0] != (int)vita2d_texture_get_width(texture) || atlas_size[1] != (int)vita2d_texture_get_height(texture)) {
        vita2d_free_texture(texture);
        return nullptr;
    }
    for (size_t i = 0; i < N; ++i) {
        Sprite sprite;
        if (!read_rect(json_object_get(entries, names[i]), sprite)) continue;
        if (sprite.x + sprite.w > atlas_size[0] || sprite.y + sprite.h > atlas_size[1]) continue;
        sprite.texture = texture;
        if (measure) measure_visible(texture, sprite);
        sprites[i] = sprite;
    }
    return texture;
}

bool complete() { return g.ready && g.images[(size_t)Image::Background] && g.images[(size_t)Image::Header]; }

}  // namespace

// An attempt that lost the manifest or the wallpaper/header (a transient app0: read failure) is repeated
// up to 4 times, at least one second apart, so the interface is not left half drawn until the next restart.
bool needs_retry() {
    return g.attempted >= 0 && !complete() && g.retries < 4 && sceKernelGetProcessTimeWide() >= g.retry_at;
}

bool load(bool dark) {
    if (g.attempted == (dark ? 1 : 0) && !needs_retry()) return g.ready;
    if (g.attempted == (dark ? 1 : 0)) ++g.retries; else g.retries = 0;
    // Textures of the previous theme may still be referenced by the last submitted frame.
    vita2d_wait_rendering_done();
    free_all();
    g.dark = dark;
    g.attempted = dark ? 1 : 0;
    json_error_t error;
    std::vector<unsigned char> manifest;
    json_t* root = read_file(std::string(kRoot) + "manifest.json", manifest)
        ? json_loadb(reinterpret_cast<const char*>(manifest.data()), manifest.size(), 0, &error) : nullptr;
    g.retry_at = sceKernelGetProcessTimeWide() + 1000000ull;
    if (!root) { std::fprintf(stderr, "ui_skin: manifest unreadable\n"); return false; }
    const std::string theme = dark ? "dark" : "light";
    const std::string base = "themes/" + theme + "/";

    json_t* colors = json_object_get(json_object_get(root, "themes"), theme.c_str());
    g.colors = parse_color(json_object_get(colors, "background"), g.background_color) &&
               parse_color(json_object_get(colors, "surface"), g.surface_color);

    json_t* nine_slice = json_object_get(root, "nine_slice");
    for (size_t i = 0; i < g.slices.size(); ++i) {
        const std::string relative = base + "9slice/" + kSliceNames[i] + ".png";
        json_t* entry = json_object_get(nine_slice, relative.c_str());
        json_t* insets = json_object_get(entry, "insets");
        json_t* size = json_object_get(entry, "size");
        int inset[4], dimension[2];
        if (!json_is_array(insets) || !json_is_array(size)) continue;
        bool valid = true;
        for (int k = 0; k < 4; ++k) valid = valid && json_int(json_array_get(insets, k), inset[k]);
        for (int k = 0; k < 2; ++k) valid = valid && json_int(json_array_get(size, k), dimension[k]);
        if (!valid || inset[0] + inset[2] >= dimension[0] || inset[1] + inset[3] >= dimension[1]) continue;
        vita2d_texture* texture = load_png(relative, false);
        if (!texture) continue;
        if ((int)vita2d_texture_get_width(texture) != dimension[0] || (int)vita2d_texture_get_height(texture) != dimension[1]) {
            vita2d_free_texture(texture);
            continue;
        }
        g.slices[i] = {texture, (float)dimension[0], (float)dimension[1], (float)inset[0], (float)inset[1], (float)inset[2], (float)inset[3]};
    }

    json_t* textures = json_object_get(root, "textures");
    const std::string image_paths[] = {base + "fixed/" + kBackgroundFile, base + "fixed/header_960x40.png",
                                       base + "fixed/active_indicator_128x4.png"};
    for (size_t i = 0; i < g.images.size(); ++i) {
        json_t* entry = json_object_get(textures, image_paths[i].c_str());
        json_t* size = json_object_get(entry, "size");
        int dimension[2];
        if (!json_is_array(size) || !json_int(json_array_get(size, 0), dimension[0]) || !json_int(json_array_get(size, 1), dimension[1])) continue;
        vita2d_texture* texture = load_png(image_paths[i], true);
        if (!texture) continue;
        if ((int)vita2d_texture_get_width(texture) != dimension[0] || (int)vita2d_texture_get_height(texture) != dimension[1]) {
            vita2d_free_texture(texture);
            continue;
        }
        g.images[i] = texture;
    }
    g.icons = load_atlas(root, base + "ui_icons_atlas.png", kIconNames, g.icon_sprites, false);
    g.buttons = load_atlas(root, base + "ps_buttons_atlas.png", kButtonNames, g.button_sprites, true);
    // Corner mask for video frames, generated once with an antialiased quarter-circle edge.
    if (vita2d_texture* mask = vita2d_create_empty_texture(16, 16)) {
        auto* pixels = static_cast<unsigned char*>(vita2d_texture_get_datap(mask));
        const size_t stride = vita2d_texture_get_stride(mask);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                const float dx = std::max(0.f, std::fabs(x + 0.5f - 8) - 0.f), dy = std::max(0.f, std::fabs(y + 0.5f - 8) - 0.f);
                const float inside = std::max(0.f, std::min(1.f, 8.5f - std::hypot(dx, dy)));   // distance from the center 8 px => radius-8 circle
                unsigned char* px = pixels + y * stride + x * 4;
                px[0] = px[1] = px[2] = 255; px[3] = (unsigned char)(255 * (1.f - inside));
            }
        vita2d_texture_set_filters(mask, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        g.corner = mask;
    }
    if (!g.corner_buttons) g.corner_buttons = load_png("corner_buttons.png", true);
    json_decref(root);
    g.ready = true;
    return true;
}

void unload() {
    vita2d_wait_rendering_done();
    free_all();
    if (g.corner_buttons) vita2d_free_texture(g.corner_buttons);
    g.corner_buttons = nullptr;
    g.attempted = -1;
    g.retries = 0;
}

bool ready() { return g.ready; }

void set_opacity(int percent) { g.opacity = std::max(0, std::min(100, percent)); }

unsigned int card_tint(bool focused, unsigned int rgb) {
    const int alpha = g.opacity * 255 / 100;
    const int shown = focused ? std::min(255, alpha + 45) : alpha;
    return (rgb & 0x00FFFFFFu) | ((unsigned int)shown << 24);
}

bool has_buttons() {
    if (!g.ready || !g.buttons) return false;
    for (const auto& sprite : g.button_sprites) if (!sprite.texture) return false;
    return true;
}

bool nine(Slice slice, float x, float y, float w, float h, unsigned int tint) {
    if (!g.ready) return false;
    const SliceTexture& s = g.slices[(size_t)slice];
    if (!s.texture || w < 1 || h < 1) return false;
    const float k = std::min(1.f, std::min(w / (s.left + s.right), h / (s.top + s.bottom)));
    const float source_w[3] = {s.left, s.width - s.left - s.right, s.right};
    const float source_h[3] = {s.top, s.height - s.top - s.bottom, s.bottom};
    const float dest_w[3] = {s.left * k, w - (s.left + s.right) * k, s.right * k};
    const float dest_h[3] = {s.top * k, h - (s.top + s.bottom) * k, s.bottom * k};
    float source_y = 0, dest_y = y;
    for (int row = 0; row < 3; ++row) {
        float source_x = 0, dest_x = x;
        for (int column = 0; column < 3; ++column) {
            if (dest_w[column] > 0 && dest_h[row] > 0) {
                const float sx = dest_w[column] / source_w[column], sy = dest_h[row] / source_h[row];
                if (tint == kWhite)
                    vita2d_draw_texture_part_scale(s.texture, dest_x, dest_y, source_x, source_y, source_w[column], source_h[row], sx, sy);
                else
                    vita2d_draw_texture_tint_part_scale(s.texture, dest_x, dest_y, source_x, source_y, source_w[column], source_h[row], sx, sy, tint);
            }
            source_x += source_w[column]; dest_x += dest_w[column];
        }
        source_y += source_h[row]; dest_y += dest_h[row];
    }
    return true;
}

bool background(float y, float height) {
    const vita2d_texture* texture = g.ready ? g.images[(size_t)Image::Background] : nullptr;
    if (!texture) return false;
    vita2d_draw_texture_part(texture, 0, y, 0, y, 960, height);
    return true;
}

bool header(float height) {
    const vita2d_texture* texture = g.ready ? g.images[(size_t)Image::Header] : nullptr;
    if (!texture) return false;
    vita2d_draw_texture_scale(texture, 0, 0, 1.f, height / (float)vita2d_texture_get_height(texture));
    return true;
}

bool corners(float x, float y, float w, float h, float radius) {
    if (!g.ready || !g.corner || w < 2 * radius || h < 2 * radius || radius <= 0) return false;
    const float k = radius / 8.f;
    const unsigned int color = (g.colors ? g.surface_color : RGBA8(255, 255, 255, 255)) | 0xFF000000u;
    // Each quadrant of the mask is one corner; quadrants hold the fill outside the rounded hole.
    vita2d_draw_texture_tint_part_scale(g.corner, x, y, 0, 0, 8, 8, k, k, color);
    vita2d_draw_texture_tint_part_scale(g.corner, x + w - radius, y, 8, 0, 8, 8, k, k, color);
    vita2d_draw_texture_tint_part_scale(g.corner, x, y + h - radius, 0, 8, 8, 8, k, k, color);
    vita2d_draw_texture_tint_part_scale(g.corner, x + w - radius, y + h - radius, 8, 8, 8, 8, k, k, color);
    return true;
}

bool corner_button(bool right, float glow, float opacity) {
    if (!g.ready || !g.corner_buttons) return false;
    const float x = right ? 960.f - 72 : 0.f, y = 544.f - 72, tx = right ? 72.f : 0.f;
    opacity = std::max(0.f, std::min(1.f, opacity));
    if (opacity < 1.f) vita2d_draw_texture_tint_part_scale(g.corner_buttons, x, y, tx, 0, 72, 72, 1.f, 1.f, RGBA8(255, 255, 255, (int)(255.f * opacity)));
    else vita2d_draw_texture_part(g.corner_buttons, x, y, tx, 0, 72, 72);
    const int alpha = (int)(255.f * std::max(0.f, std::min(1.f, glow)) * opacity);
    if (alpha > 0) vita2d_draw_texture_tint_part_scale(g.corner_buttons, x, y, tx, 72, 72, 72, 1.f, 1.f, RGBA8(255, 255, 255, alpha));
    return true;
}

bool indicator(float x, float y, float w, float h) {
    const vita2d_texture* texture = g.ready ? g.images[(size_t)Image::ActiveIndicator] : nullptr;
    if (!texture || w <= 0 || h <= 0) return false;
    vita2d_draw_texture_scale(texture, x, y, w / (float)vita2d_texture_get_width(texture), h / (float)vita2d_texture_get_height(texture));
    return true;
}

bool icon(Icon icon, float x, float y, float size, unsigned int tint) {
    if (!g.ready) return false;
    const Sprite& sprite = g.icon_sprites[(size_t)icon];
    if (!sprite.texture) return false;
    vita2d_draw_texture_tint_part_scale(sprite.texture, x, y, sprite.x, sprite.y, sprite.w, sprite.h,
                                        size / sprite.w, size / sprite.h, tint);
    return true;
}

bool button(int index, Sprite& sprite) {
    if (!g.ready || index < 0 || index >= (int)g.button_sprites.size() || !g.button_sprites[index].texture) return false;
    sprite = g.button_sprites[index];
    return true;
}

unsigned int background_color(unsigned int fallback) { return g.ready && g.colors ? g.background_color : fallback; }
unsigned int surface_color(unsigned int fallback) { return g.ready && g.colors ? g.surface_color : fallback; }

}  // namespace ui_skin
