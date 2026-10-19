#pragma once
#include <jansson.h>
#include <array>
#include <cstdint>
#include <unordered_map>

namespace peer_colors {
struct Palette {
    std::array<uint32_t, 3> rgb{};
    int count = 0;
};
struct Peer {
    int accent = -1;
    Palette light, dark;
};
inline Palette read(json_t* array) {
    Palette result;
    if (!json_is_array(array) || json_array_size(array) > 3) return result;
    size_t i; json_t* value;
    json_array_foreach(array, i, value) {
        if (!json_is_integer(value) || json_integer_value(value) < 0 || json_integer_value(value) > 0xffffff) return {};
        result.rgb[result.count++] = (uint32_t)json_integer_value(value);
    }
    return result;
}
inline Peer parse(json_t* object) {
    Peer result;
    auto* accent = json_object_get(object, "accent_color_id");
    if (json_is_integer(accent) && json_integer_value(accent) >= 0 && json_integer_value(accent) <= INT32_MAX)
        result.accent = (int)json_integer_value(accent);
    auto* gift = json_object_get(object, "upgraded_gift_colors");
    for (bool dark : {false, true}) {
        auto* color = json_object_get(gift, dark ? "dark_theme_accent_color" : "light_theme_accent_color");
        if (json_is_integer(color) && json_integer_value(color) >= 0 && json_integer_value(color) <= 0xffffff) {
            auto& palette = dark ? result.dark : result.light;
            palette.count = 1;
            palette.rgb[0] = (uint32_t)json_integer_value(color);
        }
    }
    return result;
}
class Colors {
    std::unordered_map<int, Peer> palettes_;
public:
    void update(json_t* colors) {
        palettes_.clear();
        size_t i; json_t* color;
        json_array_foreach(colors, i, color) {
            auto* id = json_object_get(color, "id");
            if (!json_is_integer(id) || json_integer_value(id) < 0 || json_integer_value(id) > INT32_MAX) continue;
            Peer palette;
            palette.light = read(json_object_get(color, "light_theme_colors"));
            palette.dark = read(json_object_get(color, "dark_theme_colors"));
            if (!palette.dark.count) palette.dark = palette.light;
            if (palette.light.count) palettes_[(int)json_integer_value(id)] = palette;
        }
    }
    Palette get(const Peer& peer, int64_t fallback_id, bool dark) const {
        const auto& gift = dark ? peer.dark : peer.light;
        if (gift.count) return gift;
        auto found = palettes_.find(peer.accent);
        if (found != palettes_.end()) return dark ? found->second.dark : found->second.light;
        int index = peer.accent;
        if (index < 0 || index > 6) {
            // Telegram chat IDs encode their type; the default color uses the bare peer ID.
            if (fallback_id <= -1000000000000LL) fallback_id += 1000000000000LL;
            index = (int)(fallback_id % 7);
            if (index < 0) index = -index;
        }
        // Telegram Android default / Dark Blue name colors (ThemeColors.java, darkblue.attheme).
        static constexpr uint32_t light[] = {0xcc5049, 0xd67722, 0x955cdb, 0x40a920, 0x309eba, 0x368ad1, 0xc7508b};
        static constexpr uint32_t night[] = {0xe88878, 0xe8ab5a, 0x8c8dea, 0x7dd071, 0x53ccc6, 0x5cadea, 0xe8749a};
        return {{{(dark ? night : light)[index], 0, 0}}, 1};
    }
};
inline uint32_t rgba(uint32_t rgb) {
    return 0xff000000u | ((rgb & 255) << 16) | (rgb & 0xff00) | (rgb >> 16);
}
}
