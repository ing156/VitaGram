#include "qr_render.h"
#include <algorithm>
#include <cmath>

namespace {
bool rounded(float x, float y, float side, float radius) {
    if (x < 0 || y < 0 || x >= side || y >= side) return false;
    const float dx = std::max(std::abs(x-side/2)-(side/2-radius), 0.f);
    const float dy = std::max(std::abs(y-side/2)-(side/2-radius), 0.f);
    return dx*dx+dy*dy <= radius*radius;
}

struct Point { float x, y; };
bool triangle(float x, float y, Point a, Point b, Point c) {
    auto cross = [=](Point p, Point q) { return (x-q.x)*(p.y-q.y)-(p.x-q.x)*(y-q.y); };
    const float u = cross(a,b), v = cross(b,c), w = cross(c,a);
    return !((u < 0 || v < 0 || w < 0) && (u > 0 || v > 0 || w > 0));
}

bool dark(const QrCode& qr, float x, float y) {
    if (x < 0 || y < 0 || x >= qr.size || y >= qr.size) return false;
    // Dedicated rounded finder rings, retaining the 1:1:3:1:1 centre cross.
    for (Point p : {Point{0,0}, Point{float(qr.size-7),0}, Point{0,float(qr.size-7)}}) {
        const float u=x-p.x, v=y-p.y;
        if (u >= 0 && v >= 0 && u < 7 && v < 7)
            return (rounded(u,v,7,1.5f) && !rounded(u-1,v-1,5,1.f)) ||
                   rounded(u-2,v-2,3,0.75f);
    }
    // Odd, module-aligned clearing, capped at 9 modules. Do not let the badge
    // grow with long payloads; it must stay within the H correction budget.
    const int logo = std::min(9, std::max(5, (qr.size/5)|1));
    const float begin = (qr.size-logo)/2;
    if (x >= begin && x < begin+logo && y >= begin && y < begin+logo) {
        const float radius=(logo-2)*0.5f;
        const float u=(x-qr.size*0.5f)/radius, v=(y-qr.size*0.5f)/radius;
        if (u*u+v*v > 1) return false;
        // White folded paper plane in a black circular badge, drawn as geometry.
        const Point tip{0.72f,-0.58f}, left{-0.72f,-0.03f}, fold{-0.23f,0.14f};
        const Point tail{0.4f,0.7f}, joint{-0.04f,0.3f};
        if (triangle(u,v,tip,left,fold) || triangle(u,v,tip,fold,tail) ||
            triangle(u,v,fold,Point{-0.26f,0.52f},joint)) return false;
        return true;
    }
    const int mx=int(x), my=int(y);
    const bool cell=qr.dark(mx,my);
    const float u=x-mx, v=y-my;
    const int dx=u < 0.5f ? -1 : 1, dy=v < 0.5f ? -1 : 1;
    const float cx=u-0.5f, cy=v-0.5f;
    const bool outside=cx*cx+cy*cy > 0.25f;
    if (cell) return !outside || qr.dark(mx+dx,my) || qr.dark(mx,my+dy);
    // Fill concave corners where three neighbouring dark modules meet.
    return outside && qr.dark(mx+dx,my) && qr.dark(mx,my+dy) && qr.dark(mx+dx,my+dy);
}
}

QrImage qr_render_login(const QrCode& qr, int module) {
    QrImage out;
    if (qr.size < 21 || qr.size > 57 || qr.modules.size() != size_t(qr.size*qr.size) ||
        module < 2 || module > 16) return out;
    out.side=(qr.size+8)*module;
    out.pixels.resize(size_t(out.side)*out.side);
    for (int y=0; y<out.side; ++y) for (int x=0; x<out.side; ++x) {
        int count=0;
        // Four coverage samples keep curves smooth without changing module pitch.
        for (float oy : {0.25f,0.75f}) for (float ox : {0.25f,0.75f})
            count += dark(qr,(x+ox)/module-4,(y+oy)/module-4);
        out.pixels[size_t(y)*out.side+x]=uint8_t(255-count*255/4);
    }
    return out;
}
