#include "download_folder_picker.h"
#include "i18n.h"
#include "file_download.h"
#include <psp2/io/dirent.h>

void DownloadFolderPicker::open(const std::string& preferred) {
    active = true;
    path = normalized_download_dir(preferred);
    if (!exists(path)) path = "ux0:downloads";
    if (!exists(path)) path = "ux0:";
    if (!exists(path)) path.clear();
    reload();
}

void DownloadFolderPicker::reload() {
    folders.clear();
    selected = 0;
    error.clear();
    if (path.empty()) {
        for (const char* root : {"ux0:", "uma0:", "imc0:", "ur0:", "grw0:"})
            if (exists(root)) folders.emplace_back(root);
        if (folders.empty()) error = tr("picker.no_drives");
        return;
    }
    std::string directory = path.back() == ':' ? path + "/" : path;
    SceUID dir = sceIoDopen(directory.c_str());
    if (dir < 0) { error = tr("picker.open_failed"); return; }
    SceIoDirent entry{};
    while (sceIoDread(dir, &entry) > 0) {
        std::string name(entry.d_name);
        if (name != "." && name != ".." && SCE_S_ISDIR(entry.d_stat.st_mode)) folders.push_back(name);
        entry = {};
    }
    sceIoDclose(dir);
}

void DownloadFolderPicker::enter() {
    if (selected < 0 || selected >= (int)folders.size()) return;
    path = path.empty() ? folders[selected] : path + (path.back() == ':' ? "" : "/") + folders[selected];
    reload();
}

void DownloadFolderPicker::up() {
    if (path.empty()) { active = false; return; }
    size_t slash = path.find_last_of('/');
    path = slash == std::string::npos ? std::string() : path.substr(0, slash);
    reload();
}

bool DownloadFolderPicker::create(const std::string& name) {
    if (path.empty() || name.empty() || name == "." || name == ".." || name.size() > 180 ||
        name.find_first_of("/\\:*?\"<>|") != std::string::npos) {
        error = tr("picker.invalid_name");
        return false;
    }
    for (unsigned char c : name) if (c < 32) { error = tr("picker.invalid_name"); return false; }
    std::string target = path + (path.back() == ':' ? "" : "/") + name;
    if (sceIoMkdir(target.c_str(), 0777) < 0) {
        error = tr("picker.create_failed");
        return false;
    }
    path = target;
    reload();
    return true;
}

bool DownloadFolderPicker::exists(const std::string& directory) {
    if (directory.empty()) return false;
    std::string target = directory.back() == ':' ? directory + "/" : directory;
    SceUID dir = sceIoDopen(target.c_str());
    if (dir < 0) return false;
    sceIoDclose(dir);
    return true;
}
