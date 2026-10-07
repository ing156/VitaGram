#pragma once
#include <psp2/io/stat.h>
#include <cstdio>
#include <string>

inline std::string normalized_download_dir(std::string path) {
    for (char& c : path) if (c == '\\') c = '/';
    const auto colon = path.find(':');
    if (colon == std::string::npos || colon < 2 || colon > 5 ||
        path.find(':', colon + 1) != std::string::npos) return {};
    for (size_t i = 0; i < colon; ++i)
        if ((path[i] < 'a' || path[i] > 'z') && (path[i] < '0' || path[i] > '9')) return {};
    path.erase(colon + 1, path.find_first_not_of('/', colon + 1) - (colon + 1));
    while (path.size() > colon + 1 && path.back() == '/') path.pop_back();
    size_t start = colon + 1;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        std::string part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || part.find_first_of("\\:*?\"<>|") != std::string::npos) return {};
        start = end + 1;
    }
    return path;
}

inline std::string safe_download_name(std::string name, int file_id) {
    for (char& c : name) if (c == '\\') c = '/';
    name = name.substr(name.find_last_of('/') == std::string::npos ? 0 : name.find_last_of('/') + 1);
    for (char& c : name) if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
                              (unsigned char)c < 32) c = '_';
    if (name.empty() || name == "." || name == "..") name = "file-" + std::to_string(file_id);
    if (name.size() > 180) name.resize(180);
    return name;
}

inline bool ensure_download_directory(const std::string& directory) {
    std::string dir = normalized_download_dir(directory);
    if (dir.empty()) return false;
    auto ensure_one = [](const std::string& path) {
        if (sceIoMkdir(path.c_str(), 0777) >= 0) return true;
        SceIoStat info{};
        return sceIoGetstat(path.c_str(), &info) >= 0 && SCE_S_ISDIR(info.st_mode);
    };
    for (size_t slash = dir.find('/'); slash != std::string::npos; slash = dir.find('/', slash + 1))
        if (!ensure_one(dir.substr(0, slash))) return false;
    return ensure_one(dir);
}

inline bool save_downloaded_file(const std::string& source, const std::string& directory,
                                 const std::string& name, int file_id, std::string& result) {
    std::string dir = normalized_download_dir(directory);
    if (dir.empty()) return false;
    if (!ensure_download_directory(dir)) return false;
    std::string clean = safe_download_name(name, file_id);
    std::string stem = clean, extension;
    size_t dot = clean.find_last_of('.');
    if (dot != std::string::npos && dot > 0) { stem = clean.substr(0, dot); extension = clean.substr(dot); }
    std::string target = dir + "/" + clean;
    SceIoStat info{};
    for (int suffix = 1; sceIoGetstat(target.c_str(), &info) >= 0; ++suffix)
        target = dir + "/" + stem + " (" + std::to_string(suffix) + ")" + extension;
    FILE* in = std::fopen(source.c_str(), "rb");
    if (!in) return false;
    FILE* out = std::fopen(target.c_str(), "wb");
    if (!out) { std::fclose(in); return false; }
    char buffer[65536];
    bool ok = true;
    while (size_t count = std::fread(buffer, 1, sizeof(buffer), in))
        if (std::fwrite(buffer, 1, count, out) != count) { ok = false; break; }
    if (std::ferror(in)) ok = false;
    if (std::fclose(out) != 0) ok = false;
    std::fclose(in);
    if (!ok) { std::remove(target.c_str()); return false; }
    result = target;
    return true;
}
