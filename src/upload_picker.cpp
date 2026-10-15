#include "upload_picker.h"
#include "i18n.h"
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/apputil.h>
#include <psp2/rtc.h>
#include <psp2/system_param.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <psp2/io/fcntl.h>

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
bool has_extension(const std::string& name, std::initializer_list<const char*> list) {
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = lower(name.substr(dot + 1));
    for (const char* e : list) if (ext == e) return true;
    return false;
}
// Local time in the console's own date and clock format.
std::string format_date(const SceDateTime& utc_time, uint64_t& key) {
    static int date_format = -1, time_format = -1;
    if (date_format < 0) {
        int value = 0;
        date_format = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_DATE_FORMAT, &value) >= 0 ? value : 0;
        time_format = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_TIME_FORMAT, &value) >= 0 ? value : 1;
    }
    SceDateTime t = utc_time;
    SceRtcTick utc{}, local{};
    if (sceRtcGetTick(&utc_time, &utc) >= 0 && sceRtcConvertUtcToLocalTime(&utc, &local) >= 0) sceRtcSetTick(&t, &local);
    key = (((((uint64_t)t.year * 13 + t.month) * 32 + t.day) * 24 + t.hour) * 60 + t.minute) * 60 + t.second;
    char date[24], clock[16];
    if (date_format == SCE_SYSTEM_PARAM_DATE_FORMAT_DDMMYYYY) std::snprintf(date, sizeof(date), "%02d/%02d/%04d", t.day, t.month, t.year);
    else if (date_format == SCE_SYSTEM_PARAM_DATE_FORMAT_MMDDYYYY) std::snprintf(date, sizeof(date), "%02d/%02d/%04d", t.month, t.day, t.year);
    else std::snprintf(date, sizeof(date), "%04d/%02d/%02d", t.year, t.month, t.day);
    if (time_format == SCE_SYSTEM_PARAM_TIME_FORMAT_12HR)
        std::snprintf(clock, sizeof(clock), "%d:%02d %s", t.hour % 12 ? t.hour % 12 : 12, t.minute, t.hour < 12 ? "AM" : "PM");
    else std::snprintf(clock, sizeof(clock), "%02d:%02d", t.hour, t.minute);
    return std::string(date) + " " + clock;
}
// Files written by system services sometimes carry only a creation time, so use the newer of the two.
std::string newest_date(const SceIoStat& st, uint64_t& key) {
    uint64_t k_m = 0, k_c = 0;
    std::string d_m = format_date(st.st_mtime, k_m), d_c = format_date(st.st_ctime, k_c);
    if (k_c > k_m) { key = k_c; return d_c; }
    key = k_m; return d_m;
}
std::string join(const std::string& dir, const std::string& name) {
    return dir + (dir.back() == ':' ? "" : "/") + name;
}
}

bool UploadPicker::matches(Kind k, const std::string& name) {
    switch (k) {
        case Kind::Photo: return has_extension(name, {"jpg", "jpeg", "png", "webp", "gif", "bmp"});
        case Kind::Video: return has_extension(name, {"mp4", "m4v", "mov", "mkv", "webm", "avi", "3gp"});
        case Kind::File: return true;
    }
    return true;
}

std::string UploadPicker::formatSize(int64_t bytes) {
    char b[32];
    if (bytes >= 1048576) std::snprintf(b, sizeof(b), "%.1f MB", bytes / 1048576.0);
    else std::snprintf(b, sizeof(b), "%.0f KB", bytes / 1024.0);
    return b;
}

// Opens a directory trying the spellings the Vita file layer accepts ("ux0:dir", "ux0:/dir", trailing slash).
// `trace` collects the error code of every attempt for the diagnostic log.
static SceUID open_directory(const std::string& path, std::string& trace) {
    std::vector<std::string> variants{path.back() == ':' ? path + "/" : path};
    const size_t colon = path.find(':');
    if (colon != std::string::npos && colon + 1 < path.size() && path[colon + 1] != '/')
        variants.push_back(path.substr(0, colon + 1) + "/" + path.substr(colon + 1));
    if (path.back() != '/' && path.back() != ':') variants.push_back(path + "/");
    SceUID last = -1;
    for (const auto& v : variants) {
        last = sceIoDopen(v.c_str());
        char code[40]; std::snprintf(code, sizeof(code), " -> 0x%08X; ", (unsigned)last);
        trace += v + code;
        if (last >= 0) return last;
    }
    SceIoStat st{};
    int rc = sceIoGetstat(path.c_str(), &st);
    char code[64]; std::snprintf(code, sizeof(code), "stat 0x%08X mode 0x%X", (unsigned)rc, rc >= 0 ? (unsigned)st.st_mode : 0u);
    trace += code;
    return last;
}

bool UploadPicker::exists(const std::string& directory) {
    if (directory.empty()) return false;
    SceUID dir = sceIoDopen((directory.back() == ':' ? directory + "/" : directory).c_str());
    if (dir < 0) return false;
    sceIoDclose(dir);
    return true;
}

void UploadPicker::openMenu() {
    stage = Stage::Menu; selected = 0; error.clear(); has_result_ = false; items.clear();
}

void UploadPicker::chooseKind(Kind k) {
    kind = k;
    stage = Stage::Browse;
    path.clear();
    history.clear();
    reload();
}

// Quick entries for the drive list: the places where screenshots and clips live.
void UploadPicker::addShortcuts() {
    // The screenshot folder is a system-managed directory that can be missing from the parent listing
    // and may fail the existence probe at times, so it is always offered; the others only if present.
    // Flat list of the newest screenshots from every game sub-folder.
    entries.push_back({LatestView, true, 0, 0, "", i18n::text("upload.latest")});
    entries.push_back({"ux0:picture/SCREENSHOT", true, 0});
    for (const char* dir : {"ux0:picture", "ux0:video", "ux0:music", "ux0:data", "ux0:downloads"})
        if (exists(dir)) entries.push_back({dir, true, 0});
    if (exists("uma0:picture/SCREENSHOT")) entries.push_back({"uma0:picture/SCREENSHOT", true, 0});
}

// Small diagnostic trail (ux0:data/vitagram/picker.log) for "the folder shows empty" reports.
// It names folders and files from the whole card in plain text, even with data encryption on, so it
// exists only in builds configured with -DVITAGRAM_PICKER_LOG=ON.
void UploadPicker::log(const std::string& text) const {
#if !VITAGRAM_PICKER_LOG
    (void)text;
    return;
#endif
    const char* file = "ux0:data/vitagram/picker.log";
    SceIoStat st{};
    if (sceIoGetstat(file, &st) >= 0 && st.st_size > 64 * 1024) sceIoRemove(file);
    SceUID fd = sceIoOpen(file, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0) return;
    std::string line = "[" + std::string(kind == Kind::Photo ? "photo" : kind == Kind::Video ? "video" : "file") +
        (show_hidden ? ",hidden" : "") + "] " + path + ": " + text + "\n";
    sceIoWrite(fd, line.data(), line.size());
    sceIoClose(fd);
}

// Recursive scan (depth <= 2) used by the "latest screenshots" view; a new screenshot lands in a game
// sub-folder, so looking through folders one by one hides it behind older folder dates.
void UploadPicker::scanLatest(const std::string& root, const std::string& directory, int depth) {
    std::string trace;
    SceUID dir = open_directory(directory, trace);
    if (dir < 0) { log("latest: cannot open " + directory + ": " + trace); return; }
    std::vector<std::string> subfolders;
    SceIoDirent item{};
    while (entries.size() < 2000 && sceIoDread(dir, &item) > 0) {
        std::string name(item.d_name);
        if (name != "." && name != "..") {
            if (SCE_S_ISDIR(item.d_stat.st_mode)) { if (depth < 2) subfolders.push_back(name); }
            else if (matches(kind, name) && (show_hidden || name[0] != '.')) {
                uint64_t key = 0;
                std::string date = newest_date(item.d_stat, key);
                std::string full = directory + "/" + name;
                entries.push_back({name, false, (int64_t)item.d_stat.st_size, key, date, full.substr(root.size() + 1), full});
            }
        }
        item = {};
    }
    sceIoDclose(dir);
    for (const auto& sub : subfolders) scanLatest(root, directory + "/" + sub, depth + 1);
}

std::string UploadPicker::fullPath(const Entry& e) const {
    if (!e.full.empty()) return e.full;
    return path.empty() ? e.name : join(path, e.name);
}

const char* UploadPicker::sortKey() const {
    static const char* keys[] = {"upload.sort_newest", "upload.sort_oldest", "upload.sort_name_asc",
                                 "upload.sort_name_desc", "upload.sort_size_asc", "upload.sort_size_desc"};
    return keys[(int)sort];
}

void UploadPicker::sortEntries() {
    const std::string focused = selected >= 0 && selected < (int)entries.size() ? fullPath(entries[selected]) : std::string();
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        if (path.empty() && kind == Kind::Photo) {
            const bool a_latest = a.dir && a.name == LatestView;
            const bool b_latest = b.dir && b.name == LatestView;
            if (a_latest != b_latest) return a_latest;
        }
        switch (sort) {
            case Sort::Newest: if (a.modified != b.modified) return a.modified > b.modified; break;
            case Sort::Oldest: if (a.modified != b.modified) return a.modified < b.modified; break;
            case Sort::SizeAsc: if (a.size != b.size) return a.size < b.size; break;
            case Sort::SizeDesc: if (a.size != b.size) return a.size > b.size; break;
            default: break;
        }
        const std::string an = lower(a.name), bn = lower(b.name);
        if (an != bn) return sort == Sort::NameDesc ? an > bn : an < bn;
        // Identical names in the latest view still have a deterministic order.
        return fullPath(a) < fullPath(b);
    });
    for (int i = 0; i < (int)entries.size(); ++i)
        if (fullPath(entries[i]) == focused) { selected = i; break; }
}

void UploadPicker::changeSort(int direction) {
    if (stage != Stage::Browse || direction == 0) return;
    sort = (Sort)(((int)sort + (int)Sort::Count + (direction < 0 ? -1 : 1)) % (int)Sort::Count);
    sortEntries();
}

void UploadPicker::reload() {
    entries.clear();
    selected = 0;
    error.clear();
    info.clear();
    if (path.empty()) {
        for (const char* root : {"ux0:", "uma0:", "imc0:", "ur0:", "grw0:"})
            if (exists(root)) entries.push_back({root, true, 0});
        addShortcuts();
        sortEntries();
        selected = 0;
        return;
    }
    if (path == LatestView) {
        for (const char* root : {"ux0:picture/SCREENSHOT", "uma0:picture/SCREENSHOT"}) scanLatest(root, root, 0);
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.modified > b.modified; });
        if (entries.size() > 300) entries.resize(300);
        sortEntries();
        selected = 0;
        if (entries.empty()) info = i18n::text("upload.dir_empty");
        std::string first;
        for (size_t i = 0; i < entries.size() && i < 3; ++i) first += entries[i].full + " [" + entries[i].date + "] ";
        log("latest: " + std::to_string(entries.size()) + " files; newest: " + first);
        return;
    }
    std::string trace;
    SceUID dir = open_directory(path, trace);
    if (dir < 0) {
        char code[16]; std::snprintf(code, sizeof(code), " (0x%08X)", (unsigned)dir);
        error = i18n::text("picker.open_failed") + code;
        log("open failed: " + trace);
        return;
    }
    SceIoDirent item{};
    int raw = 0;
    std::string skipped;
    int skipped_count = 0;
    while (entries.size() < 5000 && sceIoDread(dir, &item) > 0) {
        std::string name(item.d_name);
        const bool is_dir = SCE_S_ISDIR(item.d_stat.st_mode);
        const bool hidden = !name.empty() && name[0] == '.';
        if (name != "." && name != "..") {
            ++raw;
            if (!((show_hidden || !hidden) && (is_dir || matches(kind, name)))) {
                if (++skipped_count <= 4) skipped += (skipped.empty() ? "" : ", ") + name;
            }
        }
        if (name != "." && name != ".." && (show_hidden || !hidden) && (is_dir || matches(kind, name)))
        {
            uint64_t key = 0;
            std::string date = newest_date(item.d_stat, key);
            entries.push_back({name, is_dir, is_dir ? 0 : (int64_t)item.d_stat.st_size, key, date});
        }
        item = {};
    }
    sceIoDclose(dir);
    if (entries.empty())
        info = raw == 0 ? i18n::text("upload.dir_empty") : i18n::text("upload.filtered", {std::to_string(skipped_count), skipped});
    sortEntries();
    selected = 0;
    std::string newest;
    for (size_t i = 0; i < entries.size() && i < 3; ++i) newest += entries[i].name + " [" + entries[i].date + "] ";
    log("raw=" + std::to_string(raw) + " shown=" + std::to_string(entries.size()) + " skipped=" + skipped + " newest: " + newest);
}

void UploadPicker::enter() {
    if (stage != Stage::Browse || selected < 0 || selected >= (int)entries.size()) return;
    const Entry& e = entries[selected];
    if (e.dir) {
        history.push_back(fullPath(e));
        // Shortcuts in the drive list are absolute paths.
        path = path.empty() ? e.name : join(path, e.name);
        reload();
        return;
    }
    const std::string full = fullPath(e);
    const auto marked = std::find_if(items.begin(), items.end(), [&](const Item& item) { return item.path == full; });
    if (marked != items.end()) items.erase(marked);
    else if (items.size() < MaxItems) items.push_back({full, e.name, kind, e.size});
}

bool UploadPicker::isMarked(const Entry& entry) const {
    if (entry.dir) return false;
    const std::string full = fullPath(entry);
    return std::any_of(items.begin(), items.end(), [&](const Item& item) { return item.path == full; });
}

void UploadPicker::finishSelection() {
    if (stage != Stage::Browse || items.empty()) return;
    stage = Stage::Review;
    selected = 0;
    error.clear();
}

void UploadPicker::up() {
    if (stage != Stage::Browse) return;
    if (path.empty()) {
        if (items.empty()) { stage = Stage::Menu; selected = (int)kind; }
        else { stage = Stage::Review; selected = (int)items.size() + 1; }
        return;
    }
    size_t slash = path.find_last_of('/');
    path = slash == std::string::npos ? std::string() : path.substr(0, slash);
    reload();
    if (!history.empty()) {
        const std::string focused = history.back();
        history.pop_back();
        for (int i = 0; i < (int)entries.size(); ++i)
            if (fullPath(entries[i]) == focused) { selected = i; break; }
    }
}

void UploadPicker::toggleHidden() {
    show_hidden = !show_hidden;
    refresh();
}

// Rescan the current folder (or the drive list) and keep the cursor on the same entry.
void UploadPicker::refresh() {
    std::string focused = selected >= 0 && selected < (int)entries.size() ? fullPath(entries[selected]) : std::string();
    const int old_selected = selected;
    reload();
    selected = std::max(0, std::min(old_selected, (int)entries.size() - 1));
    for (int i = 0; i < (int)entries.size(); ++i) if (fullPath(entries[i]) == focused) { selected = i; break; }
}

bool UploadPicker::reviewActivate() {
    if (stage != Stage::Review) return false;
    if (selected == 0) return sendBatch();
    const int first_add = (int)items.size() + 1;
    if (selected >= first_add && selected < first_add + 3) {
        if (items.size() < MaxItems) chooseKind((Kind)(selected - first_add));
    }
    return false;
}

bool UploadPicker::sendBatch() {
    if (stage != Stage::Review || items.empty()) return false;
    result_ = items; has_result_ = true;
    close();
    return true;
}

void UploadPicker::removeSelected() {
    if (stage != Stage::Review || selected <= 0 || selected > (int)items.size()) return;
    items.erase(items.begin() + selected - 1);
    if (items.empty()) { stage = Stage::Menu; selected = 0; return; }
    selected = std::min(selected, (int)items.size());
}

bool UploadPicker::takeResult(std::vector<Item>& out) {
    if (!has_result_) return false;
    out = std::move(result_); result_.clear(); has_result_ = false;
    return true;
}
