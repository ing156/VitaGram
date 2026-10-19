#pragma once
#include <array>
#include <algorithm>
#include "ui_mask.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <string>
#include <vector>

namespace ui_themes {
struct Theme {
    const char* brand;
    int base; // Existing resource IDs: light=0, dark=1, VHBB=2.
    const char* palette;
};
// Persisted IDs stay stable even though retired catalogue presets are no longer listed.
inline constexpr std::array<Theme, 7> themes = {{
    {"", 0, nullptr}, {"", 1, nullptr}, {"", 2, nullptr},
    {"Little Greene", 0, "little-greene-light.json"},
    {"Little Greene", 1, "little-greene-dark.json"},
    {"black-blue", 1, "black-blue/palette.json"},
    {"black-red", 1, "black-red/palette.json"},
}};
inline constexpr std::array<int,7> saved_ids={0,1,2,3,4,6,7};
inline int from_saved_id(long long id) {
    for (size_t i=0;i<saved_ids.size();++i) if (saved_ids[i]==id) return (int)i;
    return 1;
}
inline int saved_id(int id) { return id>=0 && id<(int)saved_ids.size() ? saved_ids[id] : 1; }
inline constexpr const char* directory = "ux0:data/vitagram/palletes";
inline constexpr int max_custom = 64, max_themes = (int)themes.size() + max_custom;
struct Custom {
    std::string filename;
    int base;
    ui_mask::Palette colors{};
    uint64_t fields = 0;
};
inline bool valid_filename(const std::string& name) {
    if (name.size() <= 5 || name.size() > 101 || name[0] == '.' || name[0] == ' ') return false;
    std::string extension = name.substr(name.size() - 5);
    for (char& c : extension) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (extension != ".json" || name[name.size() - 6] == ' ' || name[name.size() - 6] == '.') return false;
    for (unsigned char c : name) if (c < 32 || c == 127 || std::strchr("<>:\"/\\|?*", c)) return false;
    json_t* utf8 = json_stringn(name.data(), name.size());
    if (!utf8) return false;
    json_decref(utf8);
    return true;
}
inline std::vector<Custom> scan_custom() {
    std::vector<Custom> result;
    mkdir(directory, 0777);
    DIR* dir = opendir(directory);
    if (!dir) return result;
    while (dirent* entry = readdir(dir)) {
        const std::string name(entry->d_name);
        if (!valid_filename(name)) continue;
        const std::string path = std::string(directory) + '/' + name;
        struct stat info{};
        if (lstat(path.c_str(), &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 8192) continue;
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) continue;
        char text[8193];
        const size_t size = std::fread(text, 1, sizeof(text), file);
        const bool complete = !std::ferror(file) && size == (size_t)info.st_size;
        std::fclose(file);
        if (!complete || size > 8192) continue;
        json_t* json = json_loadb(text, size, JSON_REJECT_DUPLICATES, nullptr);
        Custom custom{name, 1, {}, 0};
        bool valid = ui_mask::palette(json, custom.colors);
        for (const char* required : {"background", "surface", "text", "muted", "accent"})
            valid = valid && json_object_get(json, required);
        if (valid) {
            custom.base = ui_mask::luminance(custom.colors[ui_mask::role("background")]) <
                          ui_mask::luminance(custom.colors[ui_mask::role("text")]) ? 1 : 0;
            custom.fields = ui_mask::palette_fields(json);
            const auto at = std::lower_bound(result.begin(), result.end(), name,
                [](const Custom& item, const std::string& filename) { return item.filename < filename; });
            result.insert(at, std::move(custom));
            if (result.size() > max_custom) result.pop_back();
        }
        if (json) json_decref(json);
    }
    closedir(dir);
    return result;
}
inline const std::vector<Custom>& customs() {
    static const auto entries = scan_custom(); // Scan once: IDs and cached textures remain stable during a session.
    return entries;
}
inline int count() { return (int)themes.size() + (int)customs().size(); }
inline const Custom* custom(int id) {
    const int index = id - (int)themes.size();
    return index >= 0 && index < (int)customs().size() ? &customs()[index] : nullptr;
}
inline int find_file(const std::string& filename) {
    for (size_t i = 0; i < customs().size(); ++i)
        if (customs()[i].filename == filename) return (int)themes.size() + (int)i;
    return 1;
}
inline Theme get(int id) {
    if (const auto* entry = custom(id)) return {"", entry->base, nullptr};
    return themes[id >= 0 && id < (int)themes.size() ? id : 1];
}
} // namespace ui_themes
