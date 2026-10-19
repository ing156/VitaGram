#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ui_color {
inline double luminance(uint32_t color) {
    double result = 0;
    constexpr double weights[] = {0.2126, 0.7152, 0.0722};
    for (unsigned c = 0; c < 3; ++c) {
        const double value = ((color >> (c * 8)) & 255) / 255.0;
        result += weights[c] * (value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4));
    }
    return result;
}

inline double contrast(uint32_t a, uint32_t b) {
    const double first = luminance(a), second = luminance(b);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

inline uint32_t tone(uint32_t color, uint32_t top, uint32_t bottom, double minimum = 3.0) {
    auto score = [&](uint32_t value) { return std::min(contrast(value, top), contrast(value, bottom)); };
    if (score(color) >= minimum) return color;
    const uint32_t target = score(0xFF000000u) > score(0xFFFFFFFFu) ? 0xFF000000u : 0xFFFFFFFFu;
    for (unsigned step = 1; step <= 32; ++step) {
        uint32_t candidate = 0xFF000000u;
        for (unsigned c = 0; c < 3; ++c)
            candidate |= ((((color >> (c * 8)) & 255) * (32 - step) + ((target >> (c * 8)) & 255) * step) / 32) << (c * 8);
        if (score(candidate) >= minimum) return candidate;
    }
    return target;
}

inline uint32_t on(uint32_t background) {
    return luminance(background) > 0.179 ? 0xFF000000u : 0xFFFFFFFFu;
}
} // namespace ui_color
