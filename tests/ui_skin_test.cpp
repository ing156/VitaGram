#include "ui_skin.h"
#include "ui_themes.h"
#include "ui_mask.h"
#include <png.h>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <atomic>
#include <thread>
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <psp2/io/stat.h>

static std::string assets;
static unsigned long long now = 1;
static int live = 0, allocations = 0;
static std::atomic<int> reads{0}, palette_stats{0};
static std::atomic<bool> block_palette{false}, palette_entered{false};
static std::atomic<bool> fail_palette_stat{false};
static const vita2d_texture* last_draw = nullptr;
static float last_draw_y = 0;
struct DrawRect { float x, y, w, h; };
static std::vector<DrawRect> draw_rects;
unsigned long long sceKernelGetProcessTimeWide() { return now; }
void sceKernelDelayThread(unsigned) {}
int sceIoOpen(const char* name, int flags, int mode) {
    std::string path(name);
    if (path.rfind("app0:ui/", 0) == 0) path = assets + '/' + path.substr(8);
    if (std::string(name) == "app0:ui/corner_buttons.png") path = assets + "/../ui_extra/corner_buttons.png";
    return open(path.c_str(), flags, mode);
}
long long sceIoLseek(int fd, long long offset, int whence) { return lseek(fd, offset, whence); }
int sceIoRead(int fd, void* data, unsigned size) { ++reads; return (int)read(fd, data, size); }
int sceIoClose(int fd) { return close(fd); }
int sceIoGetstat(const char* path, SceIoStat* out) {
    ++palette_stats;
    if (block_palette) {
        palette_entered = true;
        while (block_palette) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (fail_palette_stat) return -EIO;
    struct stat info{};
    const int rc = stat(path, &info);
    if (!rc) out->st_size = info.st_size;
    return rc ? -errno : 0;
}
vita2d_texture* vita2d_create_empty_texture(unsigned w, unsigned h) {
    ++live; ++allocations;
    // Include padding to exercise the real Vita texture stride contract.
    return new vita2d_texture{w, h, (w + 3) / 4 * 16, std::vector<unsigned char>((w + 3) / 4 * 16 * h)};
}
vita2d_texture* vita2d_load_PNG_buffer(const void* data) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    // The production loader receives a complete PNG; find its IEND to bound libpng input.
    size_t size = 8;
    for (;;) {
        const unsigned n = (unsigned)bytes[size] << 24 | (unsigned)bytes[size + 1] << 16 |
                           (unsigned)bytes[size + 2] << 8 | bytes[size + 3];
        const bool end = !std::memcmp(bytes + size + 4, "IEND", 4);
        size += n + 12;
        if (end) break;
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    assert(png_image_begin_read_from_memory(&image, data, size));
    image.format = PNG_FORMAT_RGBA;
    auto* texture = vita2d_create_empty_texture(image.width, image.height);
    assert(png_image_finish_read(&image, nullptr, texture->pixels.data(), texture->stride, nullptr));
    png_image_free(&image);
    return texture;
}
void* vita2d_texture_get_datap(const vita2d_texture* t) { return const_cast<unsigned char*>(t->pixels.data()); }
unsigned vita2d_texture_get_stride(const vita2d_texture* t) { return t->stride; }
unsigned vita2d_texture_get_width(const vita2d_texture* t) { return t->width; }
unsigned vita2d_texture_get_height(const vita2d_texture* t) { return t->height; }
void vita2d_texture_set_filters(vita2d_texture*, unsigned, unsigned) {}
void vita2d_free_texture(vita2d_texture* t) { --live; delete t; }
void vita2d_wait_rendering_done() {}
void vita2d_draw_texture_part_scale(const vita2d_texture* t, float x,float y,float,float,float w,float h,float sx,float sy) {
    last_draw = t; last_draw_y = y; draw_rects.push_back({x,y,w*sx,h*sy});
}
void vita2d_draw_texture_tint_part_scale(const vita2d_texture* t, float x,float y,float tx,float ty,float w,float h,float sx,float sy,unsigned) {
    vita2d_draw_texture_part_scale(t,x,y,tx,ty,w,h,sx,sy);
}
void vita2d_draw_texture_part(const vita2d_texture* t, float,float y,float,float,float,float) { last_draw = t; last_draw_y = y; }
void vita2d_draw_texture_scale(const vita2d_texture* t, float,float,float,float) { last_draw = t; }
void vita2d_draw_rectangle(float,float,float,float,unsigned) {}

static void wait_palette(unsigned expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        ui_skin::reload_palette();
        if (ui_skin::surface_color(0) == expected) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::cerr << "palette timeout expected=" << expected << " actual=" << ui_skin::surface_color(0) << " stats=" << palette_stats << " reads=" << reads << '\n';
    assert(false && "palette update timed out");
}
static void write_palette(const char* text, unsigned expected) {
    std::ofstream("ux0:data/vitagram/palette.json") << text;
    now += 1000000;
    const int previous = reads;
    ui_skin::reload_palette();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (reads == previous && std::chrono::steady_clock::now() < deadline) {
        ui_skin::reload_palette();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(reads > previous);
    wait_palette(expected);
}

static void check_header_icons() {
    for (const char* name : {"header_text", "battery_low", "battery_warning", "battery_charging"}) {
        for (const char* background : {"header_top", "header_bottom"})
            assert(ui_mask::contrast(ui_skin::color(name, 0), ui_skin::color(background, 0)) >= 3.0);
    }
    ui_skin::Sprite sprite;
    for (int state = 0; state < 6; ++state) {
        assert(ui_skin::battery(state, sprite));
        const char* role = state == 0 ? "battery_low" : state == 1 ? "battery_warning" :
                           state == 5 ? "battery_charging" : "header_text";
        const uint32_t expected = ui_skin::color(role, 0);
        bool found = false;
        for (int y = sprite.y; y < sprite.y + sprite.h; ++y) for (int x = sprite.x; x < sprite.x + sprite.w; ++x) {
            const auto* p = sprite.texture->pixels.data() + y * sprite.texture->stride + x * 4;
            if (p[3] == 255 && p[0] == (expected & 255) && p[1] == ((expected >> 8) & 255) && p[2] == ((expected >> 16) & 255)) found = true;
        }
        assert(found);
    }
    assert(ui_skin::corner_button(false, 0));
    assert(last_draw_y == 472);
    assert(ui_skin::corner_button(true, 0, 1, 472) && last_draw_y == 400);
    assert(ui_skin::corner_button(false, 0, 0.8f, 422) && last_draw_y == 350);
    const uint32_t ink = ui_skin::color("icon", 0);
    for (unsigned side = 0; side < 2; ++side) {
        bool found = false;
        for (unsigned y = 0; y < 72; ++y) for (unsigned x = side * 72; x < (side + 1) * 72; ++x) {
            const auto* p = last_draw->pixels.data() + y * last_draw->stride + x * 4;
            // The menu glyph retains the source's slightly translucent highlights.
            if (p[3] >= 240 && std::abs((int)p[0] - (int)(ink & 255)) <= 16 &&
                std::abs((int)p[1] - (int)((ink >> 8) & 255)) <= 16 &&
                std::abs((int)p[2] - (int)((ink >> 16) & 255)) <= 16) found = true;
        }
        assert(found);
    }
    assert(ui_mask::contrast(ink, ui_skin::surface_color(0)) >= 4.5);
}

int main(int argc, char** argv) {
    assert(argc == 3);
    assets = argv[1];
    std::filesystem::current_path(argv[2]);
    std::filesystem::create_directories("ux0:data/vitagram");
    std::filesystem::create_directories(ui_themes::directory);
    std::filesystem::copy_file(assets + "/palettes/black-red/palette.json", std::string(ui_themes::directory) + "/Scarlet Ember.json");
    std::filesystem::copy_file(assets + "/palettes/black-blue/palette.json", std::string(ui_themes::directory) + "/Electric Night.json");
    const auto cold_begin = std::chrono::steady_clock::now();
    assert(ui_skin::load(1) && ui_skin::ready() && ui_skin::has_buttons());
    check_header_icons();
    const int semantic_reads = reads, semantic_allocations = allocations;
    const auto text_before = ui_skin::color("text", 0);
    assert(ui_skin::set_palette("{\"icon\":\"#ABCDEF\",\"notification\":\"#F020B0\",\"control_active\":\"#FFCC22\",\"control_inactive\":\"#11CC88\",\"delivery\":\"#FFE260\"}"));
    assert(ui_skin::color("text", 0) == text_before);
    for (const char* role : {"icon", "notification", "control_active", "control_inactive", "delivery"})
        assert(ui_skin::color(role, 0) != 0);
    assert(ui_skin::icon(ui_skin::Icon::Search, 0, 0, 32));
    bool icon_color_found = false;
    for (size_t i = 0; i + 3 < last_draw->pixels.size(); i += 4)
        if (last_draw->pixels[i] == 0xAB && last_draw->pixels[i+1] == 0xCD && last_draw->pixels[i+2] == 0xEF && last_draw->pixels[i+3] == 255) icon_color_found = true;
    assert(icon_color_found && reads == semantic_reads && allocations == semantic_allocations);
    const double cold_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cold_begin).count();
    assert(ui_skin::nine(ui_skin::Slice::Panel, 0, 0, 100, 50));
    const auto* panel = last_draw;
    const auto panel_before = panel->pixels;
    assert(ui_skin::nine(ui_skin::Slice::RowFocused, 0, 0, 100, 50));
    const auto* focused = last_draw;
    const auto focused_before = focused->pixels;
    const int border_reads = reads, border_allocations = allocations;
    assert(ui_skin::set_palette("{\"border\":\"#30F0B0\",\"focus\":\"#FFD54A\"}"));
    assert(panel->pixels != panel_before && focused->pixels != focused_before);
    assert(ui_skin::color("border", 0) == RGBA8(48,240,176,255));
    assert(ui_skin::color("focus", 0) == RGBA8(255,213,74,255));
    for (unsigned c = 0; c < 4; ++c) {
        assert(panel->pixels[24 * panel->stride + 24 * 4 + c] == panel_before[24 * panel->stride + 24 * 4 + c]);
        assert(focused->pixels[24 * focused->stride + 24 * 4 + c] == focused_before[24 * focused->stride + 24 * 4 + c]);
    }
    for (size_t i = 3; i < panel_before.size(); i += 4) assert(panel->pixels[i] == panel_before[i]);
    for (size_t i = 3; i < focused_before.size(); i += 4) assert(focused->pixels[i] == focused_before[i]);
    assert(reads == border_reads && allocations == border_allocations);
    assert(ui_skin::nine(ui_skin::Slice::Row, 0, 0, 100, 50));
    const auto* row = last_draw;
    const unsigned before_reads = reads, before_allocations = allocations;
    const auto before = row->pixels;
    assert(ui_skin::set_palette("{\"surface\":\"#FF0000\"}"));
    assert(ui_skin::surface_color(0) == RGBA8(255,0,0,255));
    assert((unsigned)reads == before_reads && (unsigned)allocations == before_allocations);
    assert(ui_skin::nine(ui_skin::Slice::Row, 0, 0, 100, 50) && last_draw == row);
    const auto pixel = row->pixels.data() + 24 * row->stride + 24 * 4;
    assert(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
    for (size_t i = 3; i < before.size(); i += 4) assert(before[i] == row->pixels[i]);
    for (const char* bad : {"{\"surface\":\"#GG0000\"}", "{\"surface\":\"#000000\",\"typo\":\"#000000\"}",
                            "{\"surface\":\"#000000\",\"surface\":\"#FFFFFF\"}"}) {
        assert(!ui_skin::set_palette(bad));
        assert(ui_skin::surface_color(0) == RGBA8(255,0,0,255));
    }
    write_palette("{\"surface\":\"#00FF00\"}", RGBA8(0,255,0,255));
    assert(ui_skin::surface_color(0) == RGBA8(0,255,0,255));
    block_palette = true; palette_entered = false; now += 1000000;
    ui_skin::reload_palette();
    const auto blocked_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!palette_entered && std::chrono::steady_clock::now() < blocked_deadline) {
        ui_skin::reload_palette();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(palette_entered);
    const auto poll_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) ui_skin::reload_palette();
    assert(std::chrono::steady_clock::now() - poll_start < std::chrono::milliseconds(250));
    assert(ui_skin::set_palette("{\"surface\":\"#FF0000\"}"));
    block_palette = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ui_skin::reload_palette();
    assert(ui_skin::surface_color(0) == RGBA8(255,0,0,255)); // A stale read cannot undo an explicit change.
    // Same-size edits within one timestamp tick must still apply.
    write_palette("{\"surface\":\"#0000FF\"}", RGBA8(0,0,255,255));
    assert(ui_skin::surface_color(0) == RGBA8(0,0,255,255));
    fail_palette_stat = true;
    const int previous_stats = palette_stats;
    now += 1000000; ui_skin::reload_palette();
    const auto error_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (palette_stats == previous_stats && std::chrono::steady_clock::now() < error_deadline) {
        ui_skin::reload_palette();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(palette_stats > previous_stats);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ui_skin::reload_palette();
    assert(ui_skin::surface_color(0) == RGBA8(0,0,255,255));
    fail_palette_stat = false;
    assert(ui_skin::load(0));
    wait_palette(RGBA8(0,0,255,255));
    write_palette("{broken", RGBA8(0,0,255,255));
    assert(ui_skin::surface_color(0) == RGBA8(0,0,255,255));
    std::filesystem::remove("ux0:data/vitagram/palette.json");
    now += 1000000;
    ui_skin::reload_palette();
    wait_palette(RGBA8(255,255,255,255));
    assert(ui_skin::surface_color(0) == RGBA8(255,255,255,255));
    check_header_icons();
    const int contrast_reads = reads, contrast_allocations = allocations;
    for (const char* header : {"#FFFFFF", "#000000", "#777777", "#0058A3"}) {
        const std::string values = std::string("{\"header_top\":\"") + header + "\",\"header_bottom\":\"" + header + "\",\"header_text\":\"" + header + "\"}";
        assert(ui_skin::set_palette(values.c_str()));
        check_header_icons();
    }
    assert(reads == contrast_reads && allocations == contrast_allocations);
    ui_skin::Sprite battery;
    assert(ui_skin::battery(5, battery));
    write_palette("{\"surface\":\"#FF0000\"}", RGBA8(255,0,0,255));
    double warm_ms = 0;
    for (size_t i = 3; i < ui_themes::themes.size(); ++i) {
        const auto& preset = ui_themes::get((int)i);
        std::ifstream file(assets + "/palettes/" + preset.palette);
        const std::string text((std::istreambuf_iterator<char>(file)), {});
        json_t* expected = json_loads(text.c_str(), 0, nullptr);
        assert(expected);
        const int old_reads = reads, old_allocations = allocations;
        const auto begin = std::chrono::steady_clock::now();
        assert(ui_skin::load((int)i));
        warm_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        check_header_icons();
        assert(reads == old_reads && allocations == old_allocations);
        std::array<unsigned, 5> pattern;
        assert(ui_skin::theme_colors((int)i, pattern));
        assert(reads == old_reads && allocations == old_allocations);
        uint32_t surface = 0;
        assert(ui_mask::color(json_string_value(json_object_get(expected, "surface")), surface));
        assert(ui_skin::surface_color(0) == surface);
        assert(pattern[1] == surface);
        assert(ui_skin::is_dark(false) == (preset.base != 0));
        assert(ui_skin::has_buttons() && ui_skin::battery(5, battery));
        now += 1000000;
        ui_skin::reload_palette();
        assert(ui_skin::surface_color(0) == surface); // A manual override must not replace a named preset.
        json_decref(expected);
    }
    assert(ui_themes::count() == (int)ui_themes::themes.size() + 2);
    for (int id = (int)ui_themes::themes.size(); id < ui_themes::count(); ++id) {
        const int previous_reads = reads, previous_allocations = allocations;
        assert(ui_skin::load(id));
        check_header_icons();
        assert(ui_skin::is_dark(false));
        std::array<unsigned, 5> pattern;
        assert(ui_skin::theme_colors(id, pattern));
        const auto* custom = ui_themes::custom(id);
        for (const char* role : {"icon", "notification", "control_active", "control_inactive", "delivery"}) {
            const int index = ui_mask::role(role);
            if (custom->fields & (uint64_t(1) << index)) assert(ui_skin::color(role, 0) == custom->colors[index]);
        }
        assert(pattern[0] == custom->colors[ui_mask::role("background")]);
        now += 1000000;
        ui_skin::reload_palette();
        assert(ui_skin::surface_color(0) == custom->colors[ui_mask::role("surface")]);
        assert(reads == previous_reads && allocations == previous_allocations);
    }
    assert(ui_skin::load(2));
    // Focus has a solid two-pixel outline and a transparent interior, even at minimum card opacity.
    assert(ui_skin::nine(ui_skin::Slice::FocusRing, 0, 0, 204, 42));
    const auto* ring = last_draw;
    auto rgba = [](const vita2d_texture* texture, unsigned x, unsigned y) {
        const auto* p = texture->pixels.data() + y * texture->stride + x * 4;
        return RGBA8(p[0], p[1], p[2], p[3]);
    };
    const unsigned edge = rgba(ring, ring->width/2, 1);
    assert((edge >> 24) >= 220);
    assert((rgba(ring, ring->width/2, 0) >> 24) >= 220);
    assert(rgba(ring, ring->width/2, ring->height/2) == 0);
    for (auto slice : {ui_skin::Slice::Row, ui_skin::Slice::RowFocused, ui_skin::Slice::TabActive}) {
        assert(ui_skin::nine(slice, 0, 0, 204, 42));
        const unsigned fill = rgba(last_draw, last_draw->width/2, last_draw->height/2);
        assert(ui_mask::contrast(edge, fill) >= 3.0);
        if (slice == ui_skin::Slice::RowFocused)
            for (unsigned y = 4; y < last_draw->height-4; ++y)
                assert(ui_mask::contrast(RGBA8(242,247,255,255), rgba(last_draw,last_draw->width/2,y)) >= 4.5);
    }
    // The real nine-slice draw calls partition each row: no repeated alpha blending at seams/corners.
    for (auto slice : {ui_skin::Slice::RowFocused, ui_skin::Slice::FocusRing}) {
        for (auto box : {DrawRect{0,0,940,41}, DrawRect{0,0,940,42}, DrawRect{0,0,192,42},
                         DrawRect{0,0,204,42}, DrawRect{0,0,112,48}, DrawRect{0,0,12,12}}) {
            draw_rects.clear();
            assert(ui_skin::nine(slice, box.x, box.y, box.w, box.h));
            for (int y = 0; y < box.h; ++y) for (int x = 0; x < box.w; ++x) {
                int coverage = 0;
                for (const auto& rect : draw_rects)
                    coverage += x+0.5f >= rect.x && x+0.5f < rect.x+rect.w &&
                                y+0.5f >= rect.y && y+0.5f < rect.y+rect.h;
                assert(coverage == 1);
            }
        }
    }
    assert(!ui_skin::set_palette("{\"surface\":\"#000000\"}"));
    assert(ui_skin::load(6) && ui_skin::has_buttons());
    std::cout << "Host skin: cold load " << cold_ms << " ms; average cached preset switch " << warm_ms / (ui_themes::themes.size() - 3) << " ms; zero PNG reads/allocations\n";
    ui_skin::unload();
    assert(live == 0);
    std::cout << "UI skin: real PNG masks, in-place palette changes, file updates, theme switches and cleanup passed\n";
}
