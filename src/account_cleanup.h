#pragma once
#include "config.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

// Only account-owned paths are removed. Settings (including MTProxy) stay intact.
namespace account_cleanup {
constexpr const char* marker = "ux0:data/vitagram/logout-pending";
inline bool pending() { struct stat s{}; return stat(marker, &s) == 0; }
inline bool mark() {
    FILE* f = std::fopen(marker, "wb");
    if (!f) return false;
    bool ok = std::fputs("logout\n", f) >= 0;
    if (std::fclose(f)) ok = false;
    return ok;
}
inline bool removeTree(const std::string& path) {
    struct stat s{};
    if (lstat(path.c_str(), &s)) return errno == ENOENT;
    if (!S_ISDIR(s.st_mode)) return std::remove(path.c_str()) == 0;
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    bool ok = true;
    errno = 0;
    while (auto* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name != "." && name != "..")
            if (!removeTree(path + "/" + name)) ok = false;
        errno = 0;
    }
    if (errno) ok = false;
    closedir(dir);
    if (rmdir(path.c_str())) ok = false;
    return ok;
}
inline bool purge() {
    // Call only after TDLib and all cache writers have stopped.
    bool ok = removeTree(VITAGRAM_DB_DIR);
    if (!removeTree(VITAGRAM_FILES_DIR)) ok = false;
    DIR* dir = opendir("ux0:data/vitagram");
    if (!dir) return false;
    errno = 0;
    while (auto* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.compare(0, 14, "voice-message-") == 0 ||
            name.compare(0, 9, "portrait-") == 0 || // transcoded copies of the account's videos
            name == "media-upload.log" || name == "media-upload.previous.log" ||
            name == "animation-error.log")
            if (!removeTree("ux0:data/vitagram/" + name)) ok = false;
        errno = 0;
    }
    if (errno) ok = false;
    closedir(dir);
    if (ok && std::remove(marker) && errno != ENOENT) ok = false;
    return ok;
}
}
