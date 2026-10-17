#pragma once
#include "vault.h"
#include <psp2/io/stat.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <functional>
#include <fcntl.h>
#include <unistd.h>

using DownloadProceed = std::function<bool()>;

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

// Visits the existing export names in `dir`: the clean name, then its " (n)" alternatives. Returns the
// first one `visit` accepts; otherwise `free` (optional) receives the first unused name.
template <class Visit>
inline std::string visit_download_names(const std::string& dir, const std::string& name, int file_id, Visit&& visit,
                                        std::string* free = nullptr) {
    std::string clean = safe_download_name(name, file_id);
    std::string stem = clean, extension;
    size_t dot = clean.find_last_of('.');
    if (dot != std::string::npos && dot > 0) { stem = clean.substr(0, dot); extension = clean.substr(dot); }
    std::string target = dir + "/" + clean;
    SceIoStat info{};
    for (int suffix = 1; sceIoGetstat(target.c_str(), &info) >= 0; ++suffix) {
        if (visit(target, info)) return target;
        target = dir + "/" + stem + " (" + std::to_string(suffix) + ")" + extension;
    }
    if (free) *free = target;
    return {};
}

inline bool same_download_contents(const std::string& source, const std::string& saved, const DownloadProceed& proceed = {}) {
    vault::Reader in;
    FILE* out = in.open(source) ? std::fopen(saved.c_str(), "rb") : nullptr;
    if (!out) return false;
    char a[16384], b[16384];
    bool same = true;
    for (;;) {
        if (proceed && !proceed()) { same = false; break; }
        int64_t count = in.read(a, sizeof(a));
        if (count < 0) { same = false; break; }
        if (std::fread(b, 1, (size_t)count, out) != (size_t)count || std::memcmp(a, b, (size_t)count)) { same = false; break; }
        if (count == 0) { same = std::fgetc(out) == EOF; break; }
    }
    std::fclose(out);
    return same;
}

// An earlier export of this document in `directory`, or "". With a cached `source` the contents
// are compared; before a download TDLib gives no content hash, so an equal size under the same
// name counts as the same file.
inline std::string find_saved_download(const std::string& directory, const std::string& name, int file_id,
                                       int64_t size, const std::string& source = {}, const DownloadProceed& proceed = {}) {
    std::string dir = normalized_download_dir(directory);
    if (dir.empty()) return {};
    if (!source.empty()) {
        vault::Reader in;
        if (!in.open(source)) return {};
        size = in.size();
    }
    if (size <= 0) return {};
    return visit_download_names(dir, name, file_id, [&](const std::string& path, const SceIoStat& info) {
        return !SCE_S_ISDIR(info.st_mode) && (int64_t)info.st_size == size &&
               (source.empty() || same_download_contents(source, path, proceed));
    });
}

// `existed` reports that an identical copy was already saved and nothing was written.
inline bool save_downloaded_file(const std::string& source, const std::string& directory,
                                 const std::string& name, int file_id, std::string& result, bool* existed = nullptr, const DownloadProceed& proceed = {},
                                 const std::function<void(int64_t, int64_t)>& progress = {}) {
    result.clear();
    if (existed) *existed = false;
    if (proceed && !proceed()) return false;
    std::string dir = normalized_download_dir(directory);
    if (dir.empty()) return false;
    if (!ensure_download_directory(dir)) return false;
    std::string saved = find_saved_download(dir, name, file_id, 0, source, proceed);
    if (proceed && !proceed()) return false;
    if (!saved.empty()) {
        if (existed) *existed = true;
        result = saved;
        return true;
    }
    std::string target;
    visit_download_names(dir, name, file_id, [](const std::string&, const SceIoStat&) { return false; }, &target);
    // The cached copy may be sealed; the saved file is the user's own export and is written plain.
    vault::Reader in;
    if (!in.open(source)) return false;
    // Publish only a complete export: another UI request may inspect this directory meanwhile.
    std::string temporary;
    visit_download_names(dir, ".vitagram-" + std::to_string(file_id) + ".part", file_id,
        [](const std::string&, const SceIoStat&) { return false; }, &temporary);
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) return false;
    FILE* out = ::fdopen(fd, "wb");
    if (!out) { ::close(fd); std::remove(temporary.c_str()); return false; }
    char buffer[65536];
    bool ok = true;
    int64_t copied = 0;
    for (;;) {
        if (proceed && !proceed()) { ok = false; break; }
        int64_t count = in.read(buffer, sizeof(buffer));
        if (count < 0) { ok = false; break; }
        if (count == 0) break;
        if (std::fwrite(buffer, 1, (size_t)count, out) != (size_t)count) { ok = false; break; }
        copied += count;
        if (progress) progress(copied, in.size());
    }
    if (std::fclose(out) != 0) ok = false;
    if (ok) ok = std::rename(temporary.c_str(), target.c_str()) == 0;
    if (!ok) { std::remove(temporary.c_str()); return false; }
    result = target;
    return true;
}
