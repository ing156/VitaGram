#pragma once
#include <jansson.h>
#include "ui_color.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

// Palette-independent mask coverage stays in RAM; only palette changes touch GPU pixels.
namespace ui_mask {
inline constexpr std::array<const char*, 34> roles = {
    "background", "header_top", "header_bottom", "surface", "surface_secondary", "hover", "menu",
    "selected", "accent", "focus", "text", "muted", "divider", "border", "shadow", "date", "progress",
    "green", "header_text", "highlight", "danger", "warning", "button_cross", "button_circle",
    "button_square", "button_triangle", "battery_low", "battery_warning", "battery_charging",
    "icon", "notification", "control_active", "control_inactive", "delivery"};
static_assert(roles.size() <= 64, "Palette field bits must cover every role");
using Palette = std::array<uint32_t, roles.size()>;
inline constexpr std::array<std::pair<const char*, const char*>, 4> inherited = {{
    {"icon", "text"}, {"control_active", "accent"}, {"control_inactive", "muted"}, {"delivery", "green"}}};

inline int role(const char* name) {
    if (name) for (size_t i = 0; i < roles.size(); ++i) if (!std::strcmp(name, roles[i])) return (int)i;
    return -1;
}

inline bool color(const char* text, uint32_t& out) {
    if (!text || std::strlen(text) != 7 || text[0] != '#') return false;
    unsigned value = 0;
    for (int i = 1; i < 7; ++i) {
        const char c = text[i];
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                          c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return false;
        value = value * 16 + (unsigned)digit;
    }
    out = 0xFF000000u | (value >> 16) | (value & 0xFF00u) | ((value & 255u) << 16);
    return true;
}

inline bool palette(json_t* object, Palette& out, bool complete = false) {
    if (!json_is_object(object) || json_object_size(object) == 0) return false;
    Palette next = out;
    const char* key;
    json_t* value;
    json_object_foreach(object, key, value) {
        const int index = role(key);
        if (index < 0 || !json_is_string(value) || !color(json_string_value(value), next[index])) return false;
    }
    if (complete && json_object_size(object) != roles.size()) return false;
    for (const auto& pair : inherited)
        if (!json_object_get(object, pair.first) && json_object_get(object, pair.second))
            next[role(pair.first)] = next[role(pair.second)];
    out = next;
    return true;
}

inline uint64_t palette_fields(json_t* object) {
    uint64_t fields = 0;
    for (size_t i = 0; i < roles.size(); ++i) if (json_object_get(object, roles[i])) fields |= uint64_t(1) << i;
    for (const auto& pair : inherited)
        if (json_object_get(object, pair.second)) fields |= uint64_t(1) << role(pair.first);
    return fields;
}

struct Layer {
    int x = 0, y = 0, width = 0, height = 0, color = 0;
    std::vector<uint8_t> alpha;
};

using ui_color::luminance;
using ui_color::contrast;
inline uint32_t header_tone(uint32_t color, uint32_t top, uint32_t bottom) {
    return ui_color::tone(color, top, bottom);
}

inline void header_colors(Palette& colors) {
    const auto top = colors[role("header_top")], bottom = colors[role("header_bottom")];
    colors[role("header_text")] = header_tone(colors[role("header_text")], top, bottom);
    // Battery signals need contrast against the header, independently of controls on panels.
    colors[role("battery_low")] = header_tone(colors[role("danger")], top, bottom);
    colors[role("battery_warning")] = header_tone(colors[role("warning")], top, bottom);
    colors[role("battery_charging")] = header_tone(colors[role("green")], top, bottom);
}

inline void render(uint8_t* pixels, size_t stride, int height, const std::vector<Layer>& layers, const Palette& colors) {
    std::memset(pixels, 0, stride * height);
    for (const auto& layer : layers) {
        const uint32_t color = colors[layer.color];
        for (int y = 0; y < layer.height; ++y) {
            auto* row = pixels + (y + layer.y) * stride + layer.x * 4;
            for (int x = 0; x < layer.width; ++x) {
                const unsigned alpha = layer.alpha[y * layer.width + x];
                if (!alpha) continue;
                auto* p = row + x * 4;
                if (alpha == 255 || p[3] == 0) {
                    for (unsigned c = 0; c < 3; ++c) p[c] = (uint8_t)(color >> (c * 8));
                    p[3] = (uint8_t)alpha;
                    continue;
                }
                if (p[3] == 255) {
                    for (unsigned c = 0; c < 3; ++c)
                        p[c] = (uint8_t)((((color >> (c * 8)) & 255) * alpha + p[c] * (255 - alpha) + 127) / 255);
                    continue;
                }
                const unsigned back = p[3] * (255 - alpha), total = alpha * 255 + back;
                for (unsigned c = 0; c < 3; ++c)
                    p[c] = (uint8_t)((((color >> (c * 8)) & 255) * alpha * 255 + p[c] * back + total / 2) / total);
                p[3] = (uint8_t)((total + 127) / 255);
            }
        }
    }
}
} // namespace ui_mask
