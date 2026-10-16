#include "crash_dump.h"
#include <dirent.h>
#include <sys/stat.h>
#include <zlib.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace crash_dump {
namespace {
constexpr size_t kProbe = 64 * 1024;

bool ends_with(const std::string& text, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return text.size() >= n && !text.compare(text.size() - n, n, suffix);
}

// A plain unlink leaves the clusters intact until reused; overwrite them first.
bool overwrite(const std::string& path) {
    struct stat info{};
    if (stat(path.c_str(), &info) || !S_ISREG(info.st_mode)) return false;
    FILE* file = std::fopen(path.c_str(), "r+b");
    if (!file) return false;
    std::vector<char> zeros(kProbe, 0);
    bool ok = true;
    for (int64_t left = info.st_size; left > 0 && ok;) {
        const size_t n = (size_t)std::min<int64_t>(left, (int64_t)zeros.size());
        ok = std::fwrite(zeros.data(), 1, n, file) == n;
        left -= (int64_t)n;
    }
    return std::fclose(file) == 0 && ok;
}
}

bool is_dump_name(const std::string& name) {
    return name.compare(0, 9, "psp2core-") == 0 && (ends_with(name, ".psp2dmp") || ends_with(name, ".psp2dmp.tmp"));
}

bool belongs_to(const std::string& path, const char* title_id) {
    gzFile file = gzopen(path.c_str(), "rb"); // also reads files that are not compressed
    if (!file) return false;
    std::vector<char> data(kProbe);
    const int got = gzread(file, data.data(), (unsigned)data.size());
    gzclose(file);
    if (got <= 0) return false;
    // The thread name field is NUL-terminated: "VITAGRM01" must not match "VITAGRM012".
    const std::string needle(title_id, std::strlen(title_id) + 1);
    return std::search(data.begin(), data.begin() + got, needle.begin(), needle.end()) != data.begin() + got;
}

int purge(const std::string& directory, const char* title_id) {
    std::vector<std::string> ours;
    if (DIR* dir = opendir(directory.c_str())) {
        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (!is_dump_name(name)) continue;
            const std::string path = directory + "/" + name;
            // An unfinished dump may still be compressing: its start already names the title.
            if (belongs_to(path, title_id)) ours.push_back(path);
        }
        closedir(dir);
    }
    int removed = 0;
    for (const auto& path : ours) {
        overwrite(path);
        if (std::remove(path.c_str()) == 0) ++removed;
    }
    return removed;
}
}
