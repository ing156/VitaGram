#include "radial_menu_view.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>

static size_t allocations = 0;
void* operator new(size_t size) { ++allocations; if (auto* ptr = std::malloc(size)) return ptr; throw std::bad_alloc(); }
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, size_t) noexcept { std::free(ptr); }
static std::array<vita2d_color_vertex, 24576> pool;
static size_t used = 0, meshes = 0, labels = 0, soft_edges = 0;
static unsigned theme_focus = 0xFFFFAA22;
static FILE* trace = nullptr;
static const radial::Labels* trace_labels = nullptr;
void* vita2d_pool_memalign(size_t bytes, size_t) {
    const size_t count = bytes / sizeof(vita2d_color_vertex);
    assert(used + count <= pool.size());
    auto* ptr = &pool[used]; used += count; return ptr;
}
void vita2d_draw_array(int primitive, const vita2d_color_vertex* v, size_t count) {
    assert((primitive == SCE_GXM_PRIMITIVE_TRIANGLE_FAN || primitive == SCE_GXM_PRIMITIVE_TRIANGLE_STRIP) && count >= 3);
    for (size_t i = 0; i < count; ++i) {
        assert(std::isfinite(v[i].x) && std::isfinite(v[i].y));
        assert(v[i].x >= 216 && v[i].x <= 744 && v[i].y >= 8 && v[i].y <= 536);
    }
    if (primitive == SCE_GXM_PRIMITIVE_TRIANGLE_STRIP && !(count % 2)) {
        bool coverage = true;
        for (size_t i = 0; i < count; i += 2) {
            const float dx = v[i+1].x-v[i].x, dy = v[i+1].y-v[i].y;
            if ((v[i+1].color>>24) || !(v[i].color>>24) || std::fabs(dx*dx+dy*dy-1) > 0.005f) coverage = false;
        }
        if (coverage) ++soft_edges;
    }
    if (trace) {
        std::fprintf(trace,"{\"type\":\"mesh\",\"primitive\":%d,\"vertices\":[",primitive);
        for (size_t i = 0; i < count; ++i) std::fprintf(trace,"%s[%.3f,%.3f,%u]",i ? "," : "",v[i].x,v[i].y,v[i].color);
        std::fputs("]}\n",trace);
    }
    ++meshes;
}
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned color) {
    assert(x == 0 && y == 0 && w == 960 && h == 544);
    if (trace) std::fprintf(trace,"{\"type\":\"rect\",\"x\":%.3f,\"y\":%.3f,\"w\":%.3f,\"h\":%.3f,\"color\":%u}\n",x,y,w,h,color);
}
void vita2d_draw_fill_circle(float x, float y, float radius, unsigned color) {
    assert(std::isfinite(x) && std::isfinite(y) && radius <= 5);
    if (trace) std::fprintf(trace,"{\"type\":\"circle\",\"x\":%.3f,\"y\":%.3f,\"radius\":%.3f,\"color\":%u}\n",x,y,radius,color);
}
void rich_text_draw(vita2d_pgf*, int x, int y, unsigned color, float scale, const char* text) {
    if (trace) {
        int width = 0;
        auto match = [&](const radial::Label& label) { if (label.text == text) width = label.width; };
        for (const auto& label : trace_labels->petals) match(label);
        for (const auto& label : trace_labels->details) match(label);
        for (const auto& label : trace_labels->modes) match(label);
        for (const auto& label : trace_labels->controls) match(label);
        match(trace_labels->text); match(trace_labels->gif); match(trace_labels->author); match(trace_labels->preview);
        std::fprintf(trace,"{\"type\":\"text\",\"x\":%.3f,\"y\":%d,\"scale\":%.3f,\"color\":%u,\"text\":\"%s\"}\n",x+width/2.f,y,scale,color,text);
    }
    ++labels;
}
namespace ui_skin {
unsigned color(const char* role, unsigned fallback) { return role[0] == 'f' ? theme_focus : fallback; }
unsigned card_tint(bool focused, unsigned rgb) { return rgb | (focused ? 0xEC000000u : 0xBF000000u); }
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }
int main() {
    using namespace radial;
    Input input;
    input.allowed = input.held = input.pressed = true;
    input.context = {42, 3, 55, 0, 7};
    Menu menu;
    assert(menu.update(input, 0).consume && !menu.visible());
    input.pressed = false; input.held = false;
    auto result = menu.update(input, 249999);
    assert(result.execute && result.action == Action::Text && !menu.visible());
    assert(!menu.update(input, 250000).execute);
    auto start = [&] {
        menu = Menu{}; input = Input{};
        input.allowed = input.held = input.pressed = true;
        input.context = {42, 3, 55, 0, 7};
        menu.update(input, 0); input.pressed = false;
    };
    start();
    menu.update(input, 249999); assert(!menu.visible());
    menu.update(input, 250000); assert(menu.visible());
    assert(near(menu.visual(250000).x, menu.config().outside_x));
    input.stick_y = 0;
    menu.update(input, 260000); assert(menu.action() == Action::Multimedia);
    const auto before_mode = menu.visual(300000);
    input.right = true;
    menu.update(input, 300000);
    assert(menu.mode() == Mode::Reply && menu.reply() == 55);
    assert(near(menu.visual(300000).x, before_mode.x));
    input.right = false; input.context.selected = 99;
    input.stick_x = 249; input.stick_y = 89;
    menu.update(input, 310000); assert(menu.action() == Action::Gif && menu.reply() == 55);
    input.held = false;
    const auto releasing = menu.visual(320000);
    result = menu.update(input, 320000); assert(!result.execute);
    const auto closing = menu.visual(320000);
    assert(near(closing.x, releasing.x) && near(closing.opacity, releasing.opacity));
    for (int i = 0; i < 5; ++i) assert(near(closing.petals[i], releasing.petals[i]));
    assert(!menu.update(input, 499999).execute);
    result = menu.update(input, 500000);
    assert(result.execute && result.action == Action::Gif && result.reply == 55 && result.context.topic == 7);
    assert(!menu.update(input, 510000).execute);
    // Every sector uses fixed directions while the wheel is still near the right edge.
    const std::array<std::array<int, 2>, 5> sticks{{{128,0}, {249,89}, {203,231}, {53,231}, {7,89}}};
    for (int i = 0; i < 5; ++i) {
        start(); input.stick_x = sticks[i][0]; input.stick_y = sticks[i][1];
        menu.update(input, 250000); assert(menu.action() == (Action)(i + 1));
        input.stick_x = input.stick_y = 128;
        menu.update(input, 260000); assert(menu.action() == Action::Text);
    }
    start(); input.stick_y = 82; menu.update(input, 250000); assert(menu.action() == Action::Text);
    input.stick_y = 78; menu.update(input, 260000); assert(menu.action() == Action::Multimedia);
    input.stick_y = 90; menu.update(input, 270000); assert(menu.action() == Action::Multimedia);
    input.stick_y = 100; menu.update(input, 280000); assert(menu.action() == Action::Text);
    // Sector boundary at -54 degrees: keep the old petal until six degrees beyond it.
    start(); input.stick_y = 0; menu.update(input, 250000);
    input.stick_x = 203; input.stick_y = 25; menu.update(input, 260000); assert(menu.action() == Action::Multimedia);
    input.stick_x = 218; input.stick_y = 38; menu.update(input, 270000); assert(menu.action() == Action::Gif);
    input.stick_x = 201; input.stick_y = 23; menu.update(input, 280000); assert(menu.action() == Action::Gif);
    // Circle, scope changes, lost permission, suspend, and repress while closing never execute.
    for (int reason = 0; reason < 7; ++reason) {
        start(); menu.update(input, 250000);
        if (reason == 0) input.circle = true;
        if (reason == 1) input.context.chat = 43;
        if (reason == 2) input.context.topic = 8;
        if (reason == 3) input.context.thread = 4;
        if (reason == 4) input.allowed = false;
        if (reason == 5) input.interrupted = true;
        if (reason == 6) { input.held = false; menu.update(input, 260000); input.held = input.pressed = true; }
        assert(!menu.update(input, 270000).execute);
        input.held = input.pressed = input.circle = input.interrupted = false;
        assert(!menu.update(input, 460000).execute);
        assert(!menu.update(input, 470000).execute && !menu.busy());
    }
    start(); input.context.reply = 66; menu = Menu{}; input.pressed = true;
    menu.update(input, 0); assert(menu.mode() == Mode::Reply && menu.reply() == 66);
    input.pressed = false; input.left = true;
    menu.update(input, 250000); assert(menu.mode() == Mode::NewMessage);
    input.left = false; input.right = true;
    menu.update(input, 260000); assert(menu.mode() == Mode::Reply && menu.reply() == 66);
    start();
    input.left = true; menu.update(input, 250000); assert(menu.mode() == Mode::Reply);
    input.left = false; menu.update(input, 260000);
    input.left = true; menu.update(input, 270000); assert(menu.mode() == Mode::NewMessage);
    input.left = false; input.right = true; menu.update(input, 280000); assert(menu.mode() == Mode::Reply);
    input.right = false; menu.update(input, 290000);
    input.right = true; menu.update(input, 300000); assert(menu.mode() == Mode::NewMessage);
    input.left = true; menu.update(input, 310000); assert(menu.mode() == Mode::NewMessage);
    start(); input.context.selected = 0; menu = Menu{}; input.pressed = true; menu.update(input, 0);
    input.pressed = false; input.right = true; menu.update(input, 250000); assert(menu.mode() == Mode::NewMessage);
    start(); menu.update(input, 250000); assert(!menu.update(input, 1300000).execute);
    input.held = false; assert(!menu.update(input, 1500000).execute);
    Config config; config.hold_us = 100000;
    menu = Menu(config); input = Input{}; input.allowed = input.held = input.pressed = true;
    menu.update(input, 0); input.pressed = false; menu.update(input, 100000); assert(menu.visible());
    // Full-open geometry and the actual renderer use theme colors and the frame pool only.
    start(); input.stick_y = 0; menu.update(input, 250000);
    const auto full = menu.visual(600000);
    assert(near(full.x, 480) && near(full.y, 272) && near(full.opacity, 1));
    for (float petal : full.petals) assert(near(petal, 1));
    Geometry geometry(config);
    // Translucent reaction highlights must cover each interior pixel only once.
    used = meshes = 0;
    rounded_box(geometry, 300, 130, 98, 58, 10, 0x4DABCDEFu);
    const size_t fan_count = 4 * (12 + 1) + 2;
    for (int y = 0; y < 58; ++y) for (int x = 0; x < 98; ++x) {
        const Point sample{300 + x + 0.37f, 130 + y + 0.61f};
        auto side = [&](const vita2d_color_vertex& a, const vita2d_color_vertex& b) {
            return (b.x-a.x)*(sample.y-a.y)-(b.y-a.y)*(sample.x-a.x);
        };
        int coverage = 0;
        for (size_t i = 1; i + 1 < fan_count; ++i) {
            const float a = side(pool[0], pool[i]), b = side(pool[i], pool[i+1]), c = side(pool[i+1], pool[0]);
            if ((a > 0 && b > 0 && c > 0) || (a < 0 && b < 0 && c < 0)) ++coverage;
        }
        assert(coverage <= 1);
        if (x >= 10 && x < 88 && y >= 10 && y < 48) assert(coverage == 1);
    }
    for (size_t i = 0; i < fan_count; ++i) assert(pool[i].color == 0x4DABCDEFu);
    Labels text;
    for (auto& label : text.petals) label.text = "media";
    const size_t baseline = allocations;
    for (int frame = 0; frame < 100; ++frame) {
        used = meshes = labels = soft_edges = 0;
        menu.update(input, 600000 + frame * 16667);
        draw(nullptr, menu, geometry, text, 600000, 0xFF221100, 0xFFFFFFFF, 0xFF665544, 0xFFFFFFFF, 0xFF665544);
        assert(meshes >= 30 && meshes <= 128 && labels == 15 && soft_edges >= 25);
    }
    assert(allocations == baseline);
    // Dark glass stays dark; low-contrast accents are lifted and filled glyphs remain readable.
    for (const auto& theme : std::array<std::array<unsigned,5>,4>{{
        {0xFF17110D,0xFFFFF7EF,0xFF1D1611,0xFF392F23,0xFFC0885D}, // BBR
        {0xFF140E16,0xFFF6F3FF,0xFF211540,0xFF5D36FF,0xFFC9BFDA}, // BRR2
        {0xFF241400,0xFFFFFFFF,0xFF4A3500,0xFF00DAFF,0xFFEAD8C0}, // yellow accent
        {0xFFFFFFFF,0xFF502A10,0xFFFFE7C9,0xFFEA7E08,0xFF856449}
    }}) {
        const auto& p = paint(theme[0],theme[1],theme[2],theme[3],theme[4]);
        assert(ui_color::contrast(p.accent,p.glyph) >= 4.5);
        const auto peak = blend(blend(theme[0],theme[2],0.5f),p.shine,0.22f);
        assert(ui_color::contrast(p.ink,peak) >= 4.5);
        assert(ui_color::contrast(p.muted,peak) >= 4.5);
        assert(ui_color::contrast(p.accent,peak) >= 4.5);
        if (ui_color::luminance(theme[0]) < 0.02) assert(ui_color::luminance(p.shine) < 0.08);
    }
    // Chord errors stay well below a pixel at the largest visible radius.
    assert(config.outer_radius*(1-std::cos(Pi/Geometry::CircleSteps)) < 0.05f);
    assert(config.outer_radius*(1-std::cos(Pi/(5*Geometry::ArcSteps))) < 0.05f);
    assert(config.corner_radius*(1-std::cos(Pi/(4*Geometry::CornerSteps))) < 0.05f);
    // Each nominal contour stays inside its annulus and rounds the four joining corners.
    for (int petal = 0; petal < 5; ++petal) {
        const auto& contour = geometry.petals[petal][4];
        for (int i = 0; i < Geometry::Points; ++i) {
            const auto p = contour[i], normal = geometry.normals[petal][i];
            assert(near(normal.x*normal.x+normal.y*normal.y,1));
            const float x = p.x+geometry.centers[petal].x, y = p.y+geometry.centers[petal].y;
            const float radius = std::sqrt(x*x+y*y);
            assert(radius >= config.inner_radius-0.1f && radius <= config.outer_radius+0.1f);
        }
        const auto a = contour[Geometry::ArcSteps], b = contour[Geometry::ArcSteps+2], c = contour[Geometry::ArcSteps+Geometry::CornerSteps];
        assert(std::fabs((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)) > 1);
    }
    // Optional draw trace uses the real renderer for visual review on the host.
    if (const char* path = std::getenv("RADIAL_PREVIEW")) {
        const char* titles[] = {"Мультимедиа","GIF","Файл","Голос","Стикеры"};
        const char* details[] = {"фото / видео","анимации","документ","запись","стикерпак"};
        for (int i = 0; i < 5; ++i) { text.petals[i].text = titles[i]; text.details[i].text = details[i]; }
        text.modes[0].text = "Новое"; text.controls[0].text = "L"; text.controls[1].text = "R";
        text.text.text = "Сообщение..."; text.gif.text = "GIF";
        trace_labels = &text; trace = std::fopen(path,"wb"); assert(trace);
        used = meshes = labels = 0;
        input.stick_y = 128; menu.update(input,2300000);
        draw(nullptr,menu,geometry,text,2300000,0xFFFFFFFF,0xFF452411,0xFFFFE7C9,0xFFFA8C08,0xFFF1DAC1);
        assert(!std::fclose(trace)); trace = nullptr;
    }
    std::puts("Radial menu: timing, all sectors, hysteresis, pinned reply, mode controls, interruption, animation continuity, themed drawing, subpixel curves, one-pixel edge coverage and zero frame allocations passed");
}
