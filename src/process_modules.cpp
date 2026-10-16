#include "process_modules.h"
#ifdef __vita__
#include <psp2/kernel/modulemgr.h>
#endif
#include <cctype>
#include <cstdio>

namespace process_modules {
namespace {
std::string lower(std::string text) {
    for (char& c : text) c = (char)std::tolower((unsigned char)c);
    return text;
}
}

bool foreign_path(const std::string& path, const std::string& title_id) {
    // Firmware may report "ux0:/app/..." or doubled slashes: compare a normalized form.
    std::string p;
    for (char c : lower(path)) if (c != '/' || (!p.empty() && p.back() != '/' && p.back() != ':')) p += c;
    const size_t colon = p.find(':');
    if (colon == std::string::npos) return false; // no device: nothing a plugin loader would produce
    const std::string device = p.substr(0, colon);
    // Read-only system partitions, and the application's own mount.
    if (device == "vs0" || device == "os0" || device == "sa0" || device == "pd0" || device == "app0") return false;
    // The installed application directory itself (some firmware reports the resolved path).
    if (p.compare(colon + 1, 5 + title_id.size(), "app/" + lower(title_id) + "/") == 0) return false;
    return true;
}

std::vector<std::string> foreign(const std::string& title_id) {
    std::vector<std::string> names;
#ifdef __vita__
    SceUID ids[256];
    SceSize count = sizeof(ids) / sizeof(ids[0]);
    if (sceKernelGetModuleList(0xFF, ids, &count) < 0) return names;
    // VitaGram's own executable, whatever path the firmware reports for it.
    const SceUID self = sceKernelGetModuleIdByAddr((void*)&foreign);
    for (SceSize i = 0; i < count; ++i) {
        if (ids[i] == self) continue;
        SceKernelModuleInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(ids[i], &info) < 0) continue;
        info.path[sizeof(info.path) - 1] = '\0';
        info.module_name[sizeof(info.module_name) - 1] = '\0';
        if (!foreign_path(info.path, title_id)) continue;
        std::fprintf(stderr, "Foreign module: %s (%s)\n", info.module_name, info.path);
        // The file name says more to the user than the internal module name.
        std::string path = info.path;
        const size_t slash = path.find_last_of("/:");
        names.push_back(slash == std::string::npos ? path : path.substr(slash + 1));
    }
#else
    (void)title_id;
#endif
    return names;
}

std::string summary(const std::vector<std::string>& names, size_t shown) {
    std::string text;
    for (size_t i = 0; i < names.size() && i < shown; ++i) text += (i ? ", " : "") + names[i];
    if (names.size() > shown) text += " +" + std::to_string(names.size() - shown);
    return text;
}
}
