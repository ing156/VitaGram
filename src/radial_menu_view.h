#pragma once
#include "radial_menu.h"
#include "media_render.h"
#include "ui_skin.h"
#include "ui_color.h"
#include <string>

namespace radial {
struct Label { std::string text; int width = 0; };
struct Labels {
    std::array<Label, 5> petals, details;
    std::array<Label, 2> modes, controls;
    Label text, author, preview, gif;
};
inline unsigned fade(unsigned color, float opacity) {
    return (color & 0x00FFFFFFu) | ((unsigned)(((color >> 24) & 255) * std::clamp(opacity, 0.f, 1.f)) << 24);
}
inline unsigned blend(unsigned a, unsigned b, float amount) {
    unsigned result = 0;
    for (int shift = 0; shift < 32; shift += 8)
        result |= (unsigned)(((a >> shift) & 255) * (1 - amount) + ((b >> shift) & 255) * amount) << shift;
    return result;
}
inline unsigned alpha(unsigned color, unsigned value) { return (color & 0x00FFFFFFu) | (std::min(255u,value) << 24); }
// Calibrate only when palette roles change; drawing keeps the cached colors.
struct Paint {
    unsigned shine, ink, muted, accent, glyph;
};
inline const Paint& paint(unsigned surface, unsigned ink, unsigned selected, unsigned accent, unsigned muted) {
    static std::array<unsigned,5> previous{};
    static Paint result{};
    const std::array<unsigned,5> colors{surface,ink,selected,accent,muted};
    if (colors != previous) {
        previous = colors;
        result.shine = ui_color::luminance(surface) < ui_color::luminance(ink) ? blend(surface,ink,0.14f) : 0xFFFFFFFFu;
        const unsigned peak = blend(blend(surface,selected,0.5f),result.shine,0.22f);
        result.ink = ui_color::tone(ink,surface,peak,4.5);
        result.muted = ui_color::tone(muted,surface,peak,4.5);
        result.accent = ui_color::tone(accent,surface,peak,4.5);
        result.glyph = ui_color::on(result.accent);
    }
    return result;
}
inline void label(vita2d_pgf* font, const Label& text, float x, float y, float scale, unsigned color) {
    rich_text_draw(font, (int)(x - text.width / 2.f), (int)y, color, scale, text.text.c_str());
}
inline vita2d_color_vertex* vertices(size_t count) {
    return static_cast<vita2d_color_vertex*>(vita2d_pool_memalign(count * sizeof(vita2d_color_vertex), 4));
}
// A one-screen-pixel coverage fringe softens contours even while their geometry scales.
template<class Normal>
inline void antialias(const vita2d_color_vertex* contour, int points, int stride, Normal normal) {
    const int count = (points+1)*2;
    auto* v = vertices(count); if (!v) return;
    for (int i = 0; i <= points; ++i) {
        const int index = i % points;
        const auto edge = contour[index*stride];
        const auto n = normal(index);
        v[2*i] = edge;
        v[2*i+1] = {edge.x+n.x,edge.y+n.y,edge.z,alpha(edge.color,0)};
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP,v,count);
}
inline void triangle(Point a, Point b, Point c, unsigned color) {
    auto* v = vertices(3); if (!v) return;
    v[0] = {a.x,a.y,0.5f,color}; v[1] = {b.x,b.y,0.5f,color}; v[2] = {c.x,c.y,0.5f,color};
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, 3);
}
inline void ring(const Geometry& geometry, float x, float y, float inner, float outer,
                 unsigned inside, unsigned outside, int first = 0, int steps = Geometry::CircleSteps, bool smooth = true) {
    const size_t count = (steps + 1) * 2;
    auto* v = vertices(count); if (!v) return;
    for (int i = 0; i <= steps; ++i) {
        const auto p = geometry.circle[(first + i) % Geometry::CircleSteps];
        v[2*i] = {x+p.x*inner,y+p.y*inner,0.5f,inside};
        v[2*i+1] = {x+p.x*outer,y+p.y*outer,0.5f,outside};
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, count);
    if (smooth && inner > 0 && (inside>>24))
        ring(geometry,x,y,std::max(0.f,inner-1),inner,alpha(inside,0),inside,first,steps,false);
    if (smooth && (outside>>24))
        ring(geometry,x,y,outer,outer+1,outside,alpha(outside,0),first,steps,false);
}
inline void rounded_box(const Geometry& geometry, float x, float y, float w, float h, float r, unsigned color) {
    constexpr int steps = 12, quarter = Geometry::CircleSteps/4;
    constexpr int points = 4*(steps+1), count = points+2;
    auto* v = vertices(count); if (!v) return;
    v[0] = {x+w/2,y+h/2,0.5f,color};
    auto normal = [&](int index) {
        const int corner = index/(steps+1), step = index%(steps+1);
        return geometry.circle[(3*quarter+corner*quarter+step*quarter/steps) % Geometry::CircleSteps];
    };
    for (int i = 0; i < points; ++i) {
        const int corner = i/(steps+1);
        const float cx = x+(corner < 2 ? w-r : r), cy = y+(corner == 0 || corner == 3 ? r : h-r);
        const auto p = normal(i);
        v[i+1] = {cx+p.x*r,cy+p.y*r,0.5f,color};
    }
    v[count-1] = v[1];
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN,v,count);
    antialias(v+1,points,1,normal);
}
inline void icon(vita2d_pgf* font, const Geometry& g, int kind, float x, float y, float scale,
                 unsigned color, unsigned white, const Label& gif) {
    auto box = [&](float px, float py, float w, float h, float r, unsigned ink) {
        rounded_box(g,x+px*scale,y+py*scale,w*scale,h*scale,r*scale,ink);
    };
    auto tri = [&](Point a, Point b, Point c, unsigned ink) {
        triangle({x+a.x*scale,y+a.y*scale},{x+b.x*scale,y+b.y*scale},{x+c.x*scale,y+c.y*scale},ink);
    };
    if (kind == 0) {
        box(-22,-20,44,40,6,color); box(-18,-16,36,32,3,white);
        vita2d_draw_fill_circle(x-9*scale,y-7*scale,4*scale,color);
        tri({-17,14},{-3,-5},{12,14},color); tri({-3,14},{9,-1},{18,14},color);
    } else if (kind == 1) {
        box(-21,-20,42,40,5,color);
        label(font,gif,x,y+6,0.55f,white);
    } else if (kind == 2) {
        box(-17,-22,34,44,3,color); box(-13,-18,26,36,1,white);
        tri({3,-22},{19,-6},{3,-6},color);
        for (int row = 0; row < 3; ++row) box(-9,-1+row*5,18,2,0.5f,color);
    } else if (kind == 3) {
        box(-8,-22,16,30,8,color);
        ring(g,x,y-3*scale,13*scale,16*scale,color,color,0,Geometry::CircleSteps/2);
        box(-16,-7,3,5,1,color); box(13,-7,3,5,1,color);
        box(-2,12,4,10,1,color); box(-10,21,20,3,1.5f,color);
    } else {
        box(-21,-20,42,40,8,color);
        vita2d_draw_fill_circle(x-8*scale,y-5*scale,2.7f*scale,white);
        vita2d_draw_fill_circle(x+8*scale,y-5*scale,2.7f*scale,white);
        ring(g,x,y,8*scale,10*scale,white,white,Geometry::CircleSteps*3/64,Geometry::CircleSteps*13/32);
        tri({7,20},{21,7},{7,7},white);
    }
}
inline void draw(vita2d_pgf* font, const Menu& menu, const Geometry& geometry, const Labels& labels,
                 int64_t now, unsigned surface, unsigned ink, unsigned selected, unsigned accent, unsigned border) {
    if (!menu.visible()) return;
    const auto view = menu.visual(now);
    const auto& config = menu.config();
    const unsigned glow = ui_skin::color("focus",accent);
    const unsigned muted = ui_skin::color("muted",blend(ink,surface,0.35f));
    const auto& colors = paint(surface,ink,selected,accent,muted);
    const unsigned white = colors.shine;
    ink = colors.ink; accent = colors.accent;
    vita2d_draw_rectangle(0,0,960,544,fade(0x18000000u,view.opacity));
    ring(geometry,view.x,view.y,config.outer_radius-1,config.outer_radius+14,
        fade(alpha(glow,110),view.opacity),fade(alpha(glow,0),view.opacity));
    for (int i = 0; i < 5; ++i) {
        const float unfold = view.petals[i], opacity = unfold * view.opacity;
        if (opacity <= 0) continue;
        const auto center = geometry.centers[i];
        const float x = view.x + center.x*unfold, y = view.y + center.y*unfold;
        const float scale = 0.4f+0.6f*unfold;
        const bool focused = menu.action() == (Action)(i+1);
        auto color_at = [&](int layer, float local_y) {
            const float light = std::clamp((center.y+local_y+config.outer_radius)/(2*config.outer_radius),0.f,1.f);
            if (layer < 3) return fade(alpha(glow,(focused ? 2 : 1)*(layer == 0 ? 9 : layer == 1 ? 19 : 38)),opacity);
            if (layer == 3) {
                const unsigned edge = focused ? glow : blend(ui_skin::color("border",border),glow,0.5f);
                return fade(blend(edge,white,focused ? 0.25f : 0.64f),opacity);
            }
            const unsigned body = focused ? blend(surface,selected,0.5f) : surface;
            const unsigned tint = blend(body,white,0.22f*(1-light));
            return fade(alpha(tint,(unsigned)((std::max(217u,ui_skin::card_tint(focused)>>24))*(0.98f-0.12f*light))),opacity);
        };
        for (int layer = 0; layer < Geometry::Layers; ++layer) {
            if (layer == 5) {
                constexpr int count = (Geometry::Points+1)*2;
                auto* v = vertices(count); if (!v) continue;
                for (int j = 0; j <= Geometry::Points; ++j) {
                    const auto outer = geometry.petals[i][4][j % Geometry::Points];
                    const auto inner = geometry.petals[i][5][j % Geometry::Points];
                    const float shine = std::clamp(0.65f-(center.y+outer.y)/(2*config.outer_radius),0.15f,1.f);
                    v[2*j] = {x+outer.x*scale,y+outer.y*scale,0.5f,fade(alpha(white,(unsigned)(240*shine)),opacity)};
                    v[2*j+1] = {x+inner.x*scale,y+inner.y*scale,0.5f,fade(alpha(white,0),opacity)};
                }
                vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP,v,count);
                antialias(v,Geometry::Points,2,[&](int index) { return geometry.normals[i][index]; });
                continue;
            }
            constexpr int count = Geometry::Points+2;
            auto* v = vertices(count); if (!v) continue;
            v[0] = {x,y,0.5f,color_at(layer,0)};
            for (int j = 0; j < Geometry::Points; ++j) {
                const auto p = geometry.petals[i][layer][j];
                v[j+1] = {x+p.x*scale,y+p.y*scale,0.5f,color_at(layer,p.y)};
            }
            v[count-1] = v[1];
            vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN,v,count);
            if (layer >= 3) antialias(v+1,Geometry::Points,1,[&](int index) { return geometry.normals[i][index]; });
        }
        icon(font,geometry,i,x,y-25*scale,scale,fade(accent,opacity),fade(colors.glyph,opacity),labels.gif);
        label(font,labels.petals[i],x,y+17*scale,0.57f,fade(ink,opacity));
        label(font,labels.details[i],x,y+36*scale,0.43f,fade(colors.muted,opacity));
    }
    const float radius = config.inner_radius-4;
    const bool focused = menu.action() == Action::Text;
    ring(geometry,view.x,view.y,radius,radius+13,fade(alpha(glow,focused ? 125 : 85),view.opacity),fade(alpha(glow,0),view.opacity));
    ring(geometry,view.x,view.y,radius-1.5f,radius+1.5f,fade(glow,view.opacity),fade(blend(glow,white,0.4f),view.opacity));
    constexpr int count = Geometry::CircleSteps+2;
    auto* v = vertices(count);
    if (v) {
        const unsigned body = blend(surface,white,0.08f);
        const unsigned opacity = std::max(245u,ui_skin::card_tint(focused)>>24);
        v[0] = {view.x,view.y,0.5f,fade(alpha(body,opacity),view.opacity)};
        for (int i = 0; i <= Geometry::CircleSteps; ++i) {
            const auto p = geometry.circle[i];
            const unsigned glass = blend(body,white,0.18f*(1-p.y));
            v[i+1] = {view.x+p.x*(radius-2),view.y+p.y*(radius-2),0.5f,
                fade(alpha(glass,opacity),view.opacity)};
        }
        vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN,v,count);
        antialias(v+1,Geometry::CircleSteps,1,[&](int index) { return geometry.circle[index]; });
    }
    const unsigned text_color = fade(ink,view.opacity), blue = fade(accent,view.opacity);
    const bool reply = menu.mode() == Mode::Reply;
    label(font,labels.modes[reply ? 1 : 0],view.x,view.y-56,0.67f,blue);
    label(font,labels.controls[0],view.x-57,view.y-30,0.54f,text_color);
    label(font,labels.controls[1],view.x+57,view.y-30,0.54f,text_color);
    for (int side : {-1,1}) {
        const float x = view.x+side*85, y = view.y-36;
        rounded_box(geometry,x-1,y-5,2,13,1,blue);
        triangle({x-5,y+4},{x+5,y+4},{x,y+10},blue);
    }
    rounded_box(geometry,view.x-94,view.y-8,188,48,7,blue);
    rounded_box(geometry,view.x-92.5f,view.y-6.5f,185,45,5.5f,fade(alpha(surface,218),view.opacity));
    if (reply) {
        label(font,labels.author,view.x,view.y+11,0.5f,text_color);
        label(font,labels.preview,view.x,view.y+30,0.43f,fade(colors.muted,view.opacity));
    } else label(font,labels.text,view.x,view.y+22,0.57f,fade(colors.muted,view.opacity));
    rounded_box(geometry,view.x-16,view.y+58,32,24,4,blue);
    triangle({view.x-10,view.y+80},{view.x-10,view.y+88},{view.x-2,view.y+80},blue);
    rounded_box(geometry,view.x-10,view.y+65,20,3,0.5f,fade(colors.glyph,view.opacity));
    rounded_box(geometry,view.x-10,view.y+71,14,3,0.5f,fade(colors.glyph,view.opacity));
}
} // namespace radial
