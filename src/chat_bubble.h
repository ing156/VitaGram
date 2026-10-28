#pragma once
#include <array>
#include <cmath>

namespace chat_bubble {
template<class Message>
bool joins(const Message& a, const Message& b) {
    return !a.service_message && !b.service_message && a.sender_id != 0 &&
        a.sender_id == b.sender_id && a.sender_chat == b.sender_chat && a.outgoing == b.outgoing &&
        a.date > 0 && b.date >= a.date;
}

struct Point { float x, y; };
struct Shape {
    std::array<Point, 55> points{};
    int count = 0;
    Point center{};
};
// Clockwise contour; the tail belongs to the same mesh as the translucent fill.
inline Shape shape(float x, float y, float w, float h, bool outgoing, bool before, bool after) {
    Shape result;
    result.center = {x+w/2, y+h/2};
    static const auto directions = [] {
        std::array<Point, 52> values{};
        for (int corner = 0; corner < 4; ++corner)
            for (int step = 0; step <= 12; ++step) {
                const float angle = (-90 + corner*90 + step*7.5f)*3.14159265358979323846f/180;
                values[corner*13+step] = {std::cos(angle), std::sin(angle)};
            }
        return values;
    }();
    auto add = [&](float px, float py) { result.points[result.count++] = {px, py}; };
    for (int corner = 0; corner < 4; ++corner) {
        if (!after && corner == (outgoing ? 1 : 2)) {
            if (outgoing) { add(x+w, y+h-10); add(x+w+6, y+h); add(x+w-3, y+h); }
            else { add(x+3, y+h); add(x-6, y+h); add(x, y+h-10); }
            continue;
        }
        const bool sender_side = outgoing ? corner < 2 : corner >= 2;
        const bool top = corner == 0 || corner == 3;
        const float r = sender_side && (top ? before : after) ? 3 : 10;
        const float cx = x+(corner < 2 ? w-r : r), cy = y+(top ? r : h-r);
        for (int step = 0; step <= 12; ++step) {
            const auto p = directions[corner*13+step];
            add(cx+p.x*r, cy+p.y*r);
        }
    }
    return result;
}
// Mitered outward offset for an outline that never paints over the fill.
inline Point outward(const Shape& shape, int i) {
    const auto a = shape.points[(i+shape.count-1)%shape.count];
    const auto b = shape.points[i], c = shape.points[(i+1)%shape.count];
    const float ab = std::hypot(b.x-a.x, b.y-a.y), bc = std::hypot(c.x-b.x, c.y-b.y);
    const Point n{(b.y-a.y)/ab, (a.x-b.x)/ab}, m{(c.y-b.y)/bc, (b.x-c.x)/bc};
    const float divisor = 1+n.x*m.x+n.y*m.y;
    return {(n.x+m.x)/divisor, (n.y+m.y)/divisor};
}
} // namespace chat_bubble
