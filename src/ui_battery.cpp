#include "ui_battery.h"
#include "ui_skin.h"
#include "ui_themes.h"
#include <vita2d.h>
#include <jansson.h>
#include <array>
#include <algorithm>
#include <string>

namespace ui_battery {
namespace {
struct Rect { int x = 0, y = 0, w = 0, h = 0; };
vita2d_texture* atlas = nullptr;
std::array<Rect, 6> sprites{};
int attempted_theme = -1;
constexpr std::array<const char*, 6> names = {
    "battery_0", "battery_25", "battery_50", "battery_75", "battery_100", "battery_charging"};
}

void unload() {
    vita2d_wait_rendering_done();
    if (atlas) vita2d_free_texture(atlas);
    atlas = nullptr;
    sprites = {};
    attempted_theme = -1;
}

void load(int theme) {
    theme = ui_themes::get(theme).base;
    if (attempted_theme == theme) return;
    unload();
    attempted_theme = theme; // Missing resources must not trigger disk reads every frame.
    ui_skin::Sprite shared;
    if (ui_skin::battery(0, shared)) return;
    json_error_t error{};
    json_t* manifest = json_load_file("app0:ui/manifest.json", 0, &error);
    if (!manifest) return;
    const std::string path = std::string("themes/") + (theme == 2 ? "vhbb" : theme == 1 ? "dark" : "light") + "/ui_icons_atlas.png";
    json_t* entries = json_object_get(json_object_get(json_object_get(manifest, "sprite_atlases"), path.c_str()), "sprites");
    if (json_is_object(entries)) atlas = vita2d_load_PNG_file(("app0:ui/" + path).c_str());
    if (atlas) {
        vita2d_texture_set_filters(atlas, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        const int width = vita2d_texture_get_width(atlas), height = vita2d_texture_get_height(atlas);
        for (size_t i = 0; i < names.size(); ++i) {
            json_t* entry = json_object_get(entries, names[i]);
            json_t* x = json_object_get(entry, "x"), *y = json_object_get(entry, "y");
            json_t* w = json_object_get(entry, "w"), *h = json_object_get(entry, "h");
            if (!json_is_integer(x) || !json_is_integer(y) || !json_is_integer(w) || !json_is_integer(h)) continue;
            const json_int_t sx = json_integer_value(x), sy = json_integer_value(y);
            const json_int_t sw = json_integer_value(w), sh = json_integer_value(h);
            if (sx < 0 || sy < 0 || sw <= 0 || sh <= 0 || sw > width || sh > height || sx > width-sw || sy > height-sh) continue;
            sprites[i] = {(int)sx, (int)sy, (int)sw, (int)sh};
        }
    }
    json_decref(manifest);
}

bool draw(int percent, bool charging, float x, float y, float size) {
    if (size <= 0 || (!charging && (percent < 0 || percent > 100))) return false;
    // Thresholds from manifest.json > battery_icons > display_rules.
    const size_t state = charging ? 5 : percent <= 12 ? 0 : percent <= 37 ? 1 :
        percent <= 62 ? 2 : percent <= 87 ? 3 : 4;
    ui_skin::Sprite shared;
    if (ui_skin::battery((int)state, shared)) {
        vita2d_draw_texture_part_scale(shared.texture, x, y, shared.x, shared.y, shared.w, shared.h, size/shared.w, size/shared.h);
        return true;
    }
    if (!atlas) return false;
    const Rect& r = sprites[state];
    if (r.w <= 0 || r.h <= 0) return false;
    vita2d_draw_texture_part_scale(atlas, x, y, r.x, r.y, r.w, r.h, size/r.w, size/r.h);
    return true;
}
}
