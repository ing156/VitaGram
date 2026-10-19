#include "ui_skin.h"
#include "ui_mask.h"
#include "ui_themes.h"
#include <sys/stat.h>
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
constexpr std::array<const char*, (size_t)Icon::Count> kIconNames = {"file", "back", "next", "play", "pause", "search", "check", "close", "download", "send", "trash", "leave", "reply", "forward", "clear-history"};
constexpr std::array<const char*, 9> kButtonNames = {"triangle", "square", "cross", "circle", "dpad", "START", "SELECT", "L", "R"};

struct SliceTexture {
    vita2d_texture* texture = nullptr;
    float width = 0, height = 0;
    float left = 0, top = 0, right = 0, bottom = 0;
};

struct MaskedTexture {
    vita2d_texture* texture = nullptr;
    std::vector<ui_mask::Layer> layers;
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
    vita2d_texture* corner_buttons = nullptr;
    vita2d_texture* corner = nullptr;   // 16x16: opaque outside a radius-8 rounded square, transparent inside
    vita2d_texture* icons = nullptr;
    vita2d_texture* buttons = nullptr;
    std::array<Sprite, (size_t)Icon::Count> icon_sprites;
    std::array<Sprite, 9> button_sprites;
    unsigned int background_color = 0, surface_color = 0;
    bool colors = false;
    json_t* mask_manifest = nullptr;
    ui_mask::Palette palette{}, defaults{};
    std::vector<MaskedTexture> masked;
    size_t mask_bytes = 0;
    std::array<Sprite, 6> battery_sprites{};
    unsigned long long palette_check_at = 0;
    long long palette_size = -1;
    std::string palette_text;
};

State g;
std::array<ui_mask::Palette, ui_themes::max_themes> palette_cache{};
bool palettes_ready = false;

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

void cache_palettes(json_t* assets, json_t* masks) {
    if (palettes_ready) return;
    json_t* owned = nullptr;
    if (!masks) {
        std::vector<unsigned char> bytes;
        json_error_t error{};
        if (read_file(std::string(kRoot) + "masks/manifest.json", bytes))
            owned = json_loadb(reinterpret_cast<const char*>(bytes.data()), bytes.size(), JSON_REJECT_DUPLICATES, &error);
        masks = owned;
    }
    ui_mask::Palette light{}, dark{};
    const bool valid = ui_mask::palette(json_object_get(json_object_get(masks, "palettes"), "light"), light, true) &&
                       ui_mask::palette(json_object_get(json_object_get(masks, "palettes"), "dark"), dark, true);
    if (valid) {
        for (int i = 0; i < ui_themes::count(); ++i) {
            const auto& preset = ui_themes::get((int)i);
            auto colors = preset.base == 0 ? light : dark;
            if (preset.base == 2)
                ui_mask::palette(json_object_get(json_object_get(assets, "themes"), "vhbb"), colors);
            if (preset.palette) {
                std::vector<unsigned char> bytes;
                json_error_t error{};
                json_t* values = read_file(std::string(kRoot) + "palettes/" + preset.palette, bytes)
                    ? json_loadb(reinterpret_cast<const char*>(bytes.data()), bytes.size(), JSON_REJECT_DUPLICATES, &error) : nullptr;
                if (!ui_mask::palette(values, colors)) std::fprintf(stderr, "ui_skin: invalid preset %s\n", preset.palette);
                if (values) json_decref(values);
            }
            if (const auto* custom = ui_themes::custom(i))
                for (size_t role = 0; role < colors.size(); ++role)
                    if (custom->fields & (uint64_t(1) << role)) colors[role] = custom->colors[role];
            ui_mask::header_colors(colors);
            palette_cache[i] = colors;
        }
        palettes_ready = true;
    }
    if (owned) json_decref(owned);
}

vita2d_texture* raw_png(const std::string& relative) {
    std::vector<unsigned char> bytes;
    return read_file(std::string(kRoot) + relative, bytes) ? vita2d_load_PNG_buffer(bytes.data()) : nullptr;
}

void free_texture(vita2d_texture* texture) {
    auto& masks = g.masked;
    for (auto it = masks.begin(); it != masks.end(); ++it) if (it->texture == texture) {
        for (const auto& layer : it->layers) g.mask_bytes -= layer.alpha.size();
        masks.erase(it);
        break;
    }
    if (texture) vita2d_free_texture(texture);
}

vita2d_texture* masked_png(const std::string& relative) {
    std::string name = relative;
    if (name.rfind("themes/", 0) == 0) name = name.substr(name.find('/', 7) + 1);
    json_t* entry = json_object_get(json_object_get(g.mask_manifest, "assets"), name.c_str());
    json_t* size = json_object_get(entry, "size");
    json_t* layers = json_object_get(entry, "layers");
    if (!json_is_array(size) || json_array_size(size) != 2 || !json_is_array(layers) ||
        json_array_size(layers) == 0 || json_array_size(layers) > 256) return nullptr;
    const int width = (int)json_integer_value(json_array_get(size, 0));
    const int height = (int)json_integer_value(json_array_get(size, 1));
    if (width <= 0 || height <= 0 || width > 1024 || height > 544) return nullptr;
    MaskedTexture result;
    size_t bytes = 0;
    for (size_t i = 0; i < json_array_size(layers); ++i) {
        json_t* layer = json_array_get(layers, i);
        const char* path = json_string_value(json_object_get(layer, "mask"));
        const int role = ui_mask::role(json_string_value(json_object_get(layer, "role")));
        json_t* rect = json_object_get(layer, "rect");
        if (!path || std::string(path).rfind("masks/", 0) != 0 || std::string(path).find("..") != std::string::npos ||
            role < 0 || !json_is_array(rect) || json_array_size(rect) != 4) return nullptr;
        int r[4];
        for (int k = 0; k < 4; ++k) {
            json_t* v = json_array_get(rect, k);
            if (!json_is_integer(v) || json_integer_value(v) < 0 || json_integer_value(v) > 1024) return nullptr;
            r[k] = (int)json_integer_value(v);
        }
        if (r[2] <= 0 || r[3] <= 0 || r[0] + r[2] > width || r[1] + r[3] > height) return nullptr;
        bytes += (size_t)r[2] * r[3];
        if (g.mask_bytes + bytes > (16u << 20)) return nullptr;
        vita2d_texture* mask = raw_png(path);
        if (!mask) return nullptr;
        if (vita2d_texture_get_width(mask) != (unsigned)r[2] || vita2d_texture_get_height(mask) != (unsigned)r[3]) {
            vita2d_free_texture(mask);
            return nullptr;
        }
        ui_mask::Layer part{r[0], r[1], r[2], r[3], role, {}};
        part.alpha.resize((size_t)r[2] * r[3]);
        const auto* pixels = static_cast<const unsigned char*>(vita2d_texture_get_datap(mask));
        const size_t stride = vita2d_texture_get_stride(mask);
        for (int y = 0; y < r[3]; ++y) for (int x = 0; x < r[2]; ++x)
            part.alpha[y * r[2] + x] = pixels[y * stride + x * 4 + 3];
        vita2d_free_texture(mask);
        result.layers.push_back(std::move(part));
    }
    result.texture = vita2d_create_empty_texture(width, height);
    if (!result.texture) return nullptr;
    ui_mask::render(static_cast<uint8_t*>(vita2d_texture_get_datap(result.texture)),
                    vita2d_texture_get_stride(result.texture), height, result.layers, g.palette);
    vita2d_texture* texture = result.texture;
    g.masked.push_back(std::move(result));
    g.mask_bytes += bytes;
    return texture;
}

void apply_palette(ui_mask::Palette colors) {
    ui_mask::header_colors(colors);
    if (g.palette != colors) {
        vita2d_wait_rendering_done();
        for (const auto& mask : g.masked) {
            const bool changed = std::any_of(mask.layers.begin(), mask.layers.end(),
                [&](const ui_mask::Layer& layer) { return colors[layer.color] != g.palette[layer.color]; });
            if (changed) ui_mask::render(static_cast<uint8_t*>(vita2d_texture_get_datap(mask.texture)),
                            vita2d_texture_get_stride(mask.texture), vita2d_texture_get_height(mask.texture), mask.layers, colors);
        }
        g.palette = colors;
    }
    g.background_color = colors[ui_mask::role("background")];
    g.surface_color = colors[ui_mask::role("surface")];
    g.colors = true;
}

vita2d_texture* load_png(const std::string& relative, bool linear) {
    const std::string path = std::string(kRoot) + relative;
    vita2d_texture* texture = g.mask_manifest ? masked_png(relative) : nullptr;
    if (!texture) texture = raw_png(relative);
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
    return json_is_string(value) && ui_mask::color(json_string_value(value), out);
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
    for (auto& slice : g.slices) { if (slice.texture) free_texture(slice.texture); slice = {}; }
    for (auto& image : g.images) { if (image) free_texture(image); image = nullptr; }
    if (g.corner_buttons) free_texture(g.corner_buttons);
    g.corner_buttons = nullptr;
    if (g.corner) vita2d_free_texture(g.corner);
    g.corner = nullptr;
    if (g.icons) free_texture(g.icons);
    if (g.buttons) free_texture(g.buttons);
    g.icons = g.buttons = nullptr;
    for (auto& sprite : g.icon_sprites) sprite = {};
    for (auto& sprite : g.button_sprites) sprite = {};
    g.battery_sprites = {};
    g.masked.clear();
    g.mask_bytes = 0;
    if (g.mask_manifest) json_decref(g.mask_manifest);
    g.mask_manifest = nullptr;
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
        free_texture(texture);
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

bool load(int theme_id) {
    theme_id = std::max(0, std::min(ui_themes::count() - 1, theme_id));
    const auto& preset = ui_themes::get(theme_id);
    const int base_id = preset.base;
    if (g.attempted == theme_id && !needs_retry()) return g.ready;
    if (g.attempted != theme_id && complete() && g.mask_manifest && g.masked.size() == (size_t)Slice::Count + (size_t)Image::Count + 3 && base_id != 2 && palettes_ready) {
        // All light/dark presets share geometry: keep mask coverage, textures and sprite coordinates.
        g.attempted = theme_id;
        g.retries = 0;
        g.defaults = palette_cache[theme_id];
        g.palette_size = -2;
        g.palette_text.clear();
        g.palette_check_at = 0;
        apply_palette(g.defaults);
        reload_palette();
        return true;
    }

    if (g.attempted == theme_id) ++g.retries; else g.retries = 0;
    // Textures of the previous theme may still be referenced by the last submitted frame.
    vita2d_wait_rendering_done();
    free_all();
    g.dark = base_id != 0;
    g.attempted = theme_id;
    json_error_t error;
    std::vector<unsigned char> manifest;
    json_t* root = read_file(std::string(kRoot) + "manifest.json", manifest)
        ? json_loadb(reinterpret_cast<const char*>(manifest.data()), manifest.size(), 0, &error) : nullptr;
    g.retry_at = sceKernelGetProcessTimeWide() + 1000000ull;
    if (!root) { std::fprintf(stderr, "ui_skin: manifest unreadable\n"); return false; }
    const std::string theme = base_id == 2 ? "vhbb" : base_id == 1 ? "dark" : "light";
    const std::string base = "themes/" + theme + "/";

    if (base_id != 2) {
        std::vector<unsigned char> masks;
        if (read_file(std::string(kRoot) + "masks/manifest.json", masks))
            g.mask_manifest = json_loadb(reinterpret_cast<const char*>(masks.data()), masks.size(), JSON_REJECT_DUPLICATES, &error);
        json_t* defaults = json_object_get(json_object_get(g.mask_manifest, "palettes"), theme.c_str());
        if (!g.mask_manifest || !ui_mask::palette(defaults, g.defaults, true)) {
            if (g.mask_manifest) json_decref(g.mask_manifest);
            g.mask_manifest = nullptr;
        } else {
            cache_palettes(root, g.mask_manifest);
            if (palettes_ready) g.defaults = palette_cache[theme_id];
            g.palette = g.defaults;
            g.palette_size = -2;
            g.palette_text.clear();
            g.palette_check_at = 0;
        }
    }

    cache_palettes(root, g.mask_manifest);

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
            free_texture(texture);
            continue;
        }
        g.slices[i] = {texture, (float)dimension[0], (float)dimension[1], (float)inset[0], (float)inset[1], (float)inset[2], (float)inset[3]};
    }

    json_t* textures = json_object_get(root, "textures");
    const std::string image_paths[] = {base + "fixed/" + kBackgroundFile, base + "fixed/header_960x40.png",
                                       base + "fixed/active_indicator_128x4.png",
                                       "themes/" + std::string(base_id == 0 ? "light" : "dark") + "/fixed/focus_edge_glow_16x128.png"};
    for (size_t i = 0; i < g.images.size(); ++i) {
        json_t* entry = json_object_get(textures, image_paths[i].c_str());
        json_t* size = json_object_get(entry, "size");
        int dimension[2];
        if (!json_is_array(size) || !json_int(json_array_get(size, 0), dimension[0]) || !json_int(json_array_get(size, 1), dimension[1])) continue;
        vita2d_texture* texture = load_png(image_paths[i], true);
        if (!texture) continue;
        if ((int)vita2d_texture_get_width(texture) != dimension[0] || (int)vita2d_texture_get_height(texture) != dimension[1]) {
            free_texture(texture);
            continue;
        }
        g.images[i] = texture;
    }
    g.icons = load_atlas(root, base + "ui_icons_atlas.png", kIconNames, g.icon_sprites, false);
    g.buttons = load_atlas(root, base + "ps_buttons_atlas.png", kButtonNames, g.button_sprites, true);
    constexpr const char* battery_names[] = {"battery_0", "battery_25", "battery_50", "battery_75", "battery_100", "battery_charging"};
    json_t* battery_entries = json_object_get(json_object_get(json_object_get(root, "sprite_atlases"), (base + "ui_icons_atlas.png").c_str()), "sprites");
    if (g.icons) for (size_t i = 0; i < g.battery_sprites.size(); ++i) {
        Sprite sprite;
        if (read_rect(json_object_get(battery_entries, battery_names[i]), sprite) && sprite.x >= 0 && sprite.y >= 0 &&
            sprite.x + sprite.w <= vita2d_texture_get_width(g.icons) && sprite.y + sprite.h <= vita2d_texture_get_height(g.icons)) {
            sprite.texture = g.icons;
            g.battery_sprites[i] = sprite;
        }
    }

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
    if (g.mask_manifest) { apply_palette(g.palette); reload_palette(); }
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

bool set_palette(const char* json) {
    if (!g.ready || !g.mask_manifest || !json || std::strlen(json) > 8192) return false;
    json_error_t error{};
    json_t* object = json_loads(json, JSON_REJECT_DUPLICATES, &error);
    auto next = g.palette;
    const bool valid = ui_mask::palette(object, next);
    if (object) json_decref(object);
    if (valid) apply_palette(next);
    return valid;
}

void reload_palette() {
    if (!g.ready || !g.mask_manifest) return;
    if (ui_themes::get(g.attempted).palette || ui_themes::custom(g.attempted)) return;
    const auto now = sceKernelGetProcessTimeWide();
    if (now < g.palette_check_at) return;
    g.palette_check_at = now + 1000000;
    constexpr const char* path = "ux0:data/vitagram/palette.json";
    struct stat info{};
    const bool exists = stat(path, &info) == 0;
    const long long size = exists ? info.st_size : -1;
    if (!exists && g.palette_size == -1) return;
    g.palette_size = size;
    auto next = g.defaults;
    if (exists) {
        if (size <= 0 || size > 8192) return;
        std::vector<unsigned char> bytes;
        if (!read_file(path, bytes)) return;
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (text == g.palette_text) return;
        g.palette_text = text;
        json_error_t error{};
        json_t* object = json_loadb(reinterpret_cast<const char*>(bytes.data()), bytes.size(), JSON_REJECT_DUPLICATES, &error);
        const bool valid = ui_mask::palette(object, next);
        if (object) json_decref(object);
        if (!valid) return;
    } else g.palette_text.clear();
    apply_palette(next);
}

unsigned int color(const char* name, unsigned int fallback) {
    const int role = ui_mask::role(name);
    return g.ready && g.mask_manifest && role >= 0 ? g.palette[role] : fallback;
}

bool theme_colors(int theme, std::array<unsigned, 5>& colors) {
    if (!palettes_ready || theme < 0 || theme >= ui_themes::count()) return false;
    constexpr const char* names[] = {"background", "surface", "accent", "text", "selected"};
    for (size_t i = 0; i < colors.size(); ++i) colors[i] = palette_cache[theme][ui_mask::role(names[i])];
    return true;
}

bool is_dark(bool fallback) {
    if (!g.ready || !g.mask_manifest) return fallback;
    auto light = [](unsigned c) { return (c & 255) * 2126 + ((c >> 8) & 255) * 7152 + ((c >> 16) & 255) * 722; };
    return light(g.palette[ui_mask::role("background")]) < light(g.palette[ui_mask::role("text")]);
}

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

bool edge_glow(bool right, float edge, float y, float height) {
    if (height <= 0) return false;
    const vita2d_texture* texture = g.ready ? g.images[(size_t)Image::EdgeGlow] : nullptr;
    // The asset already fades from alpha 150 to 1. Draw it whole: shrinking
    // all 16 columns into each pixel averaged away the bright edge.
    constexpr float width = 32.f;
    const float outer = right ? edge + 4.f : edge - 4.f;
    if (texture)
        vita2d_draw_texture_tint_part_scale(texture, outer, y - 3, 0, 0, 16, 128,
            (right ? -width : width) / 16.f, (height + 6.f) / 128.f, kWhite);
    else {
        for (int column = 0; column < (int)width; ++column) {
            const float fade = 1.f - column / width;
            vita2d_draw_rectangle(right ? outer - column - 1 : outer + column,
                y - 3, 1, height + 6, RGBA8(30, 160, 255, (int)(150.f * fade * fade)));
        }
    }
    // A pale core stays visible against the blue active tab. Taper its ends
    // and keep it on the border, clear of the folder label.
    const unsigned core = color("focus", RGBA8(160, 235, 255, 255));
    for (int band = 0; band < 16; ++band) {
        const float vertical = std::sin((band + 0.5f) * 3.14159265f / 16.f);
        vita2d_draw_rectangle(edge - 1, y + height * band / 16.f, 2, height / 16.f,
            (core & 0x00FFFFFFu) | ((unsigned)(220.f * vertical) << 24));
    }
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

bool battery(int index, Sprite& sprite) {
    if (!g.ready || index < 0 || index >= (int)g.battery_sprites.size() || !g.battery_sprites[index].texture) return false;
    sprite = g.battery_sprites[index];
    return true;
}

unsigned int background_color(unsigned int fallback) { return g.ready && g.colors ? g.background_color : fallback; }
unsigned int surface_color(unsigned int fallback) { return g.ready && g.colors ? g.surface_color : fallback; }

}  // namespace ui_skin
