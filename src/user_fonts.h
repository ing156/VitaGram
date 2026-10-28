#pragma once
#include "utf8_decode.h"
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace user_fonts {
inline constexpr const char* directory = "ux0:data/vitagram/fonts";
inline constexpr size_t max_bytes = 8 * 1024 * 1024;
inline constexpr size_t max_fonts = 64;
inline bool valid_filename(const std::string& name) {
    if (name.size() <= 4 || name.size() > 128 || name.front() == '.' || name.front() == ' ') return false;
    for (unsigned char c : name) if (c < 32 || c == 127 || std::strchr("<>:\"/\\|?*", c)) return false;
    for (const char* p = name.c_str(); *p;) {
        uint32_t code;
        const int size = decode_utf8_scalar(p, code);
        if (size == 1 && static_cast<unsigned char>(*p) >= 0x80) return false;
        p += size;
    }
    std::string extension = name.substr(name.size() - 4);
    for (char& c : extension) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return extension == ".ttf" || extension == ".otf";
}
inline std::string path(const std::string& filename) { return std::string(directory) + '/' + filename; }
inline bool valid_file(const std::string& path) {
    struct stat info{};
    return !lstat(path.c_str(), &info) && S_ISREG(info.st_mode) && info.st_size > 0 &&
        static_cast<unsigned long long>(info.st_size) <= max_bytes;
}
inline std::vector<std::string> scan() {
    std::vector<std::string> result;
    mkdir(directory, 0777);
    DIR* dir = opendir(directory);
    if (!dir) return result;
    while (dirent* entry = readdir(dir)) {
        const std::string name(entry->d_name);
        if (!valid_filename(name)) continue;
        if (!valid_file(path(name))) continue;
        result.insert(std::lower_bound(result.begin(), result.end(), name), name);
        if (result.size() > max_fonts) result.pop_back();
    }
    closedir(dir);
    return result;
}
}
