#include "i18n.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#ifdef __vita__
#include <psp2/apputil.h>
#include <psp2/io/dirent.h>
#include <psp2/system_param.h>
#else
#include <dirent.h>
#endif

namespace i18n {
namespace {

using Table = std::unordered_map<std::string, std::string>;

struct State {
    std::mutex mutex;
    std::string directory;
    std::vector<Language> languages;
    std::string current = "en";
    std::shared_ptr<const Table> active = std::make_shared<Table>();
    std::shared_ptr<const Table> english = active;
};

State& state() {
    static State s;
    return s;
}

// Controller icons are private-use code points drawn by media_render.cpp.
const char* icon_glyph(const std::string& name) {
    static const struct { const char* name; const char* glyph; } icons[] = {
        {"triangle", ""}, {"square", ""}, {"cross", ""}, {"circle", ""},
        {"dpad", ""}, {"start", ""}, {"select", ""},
    };
    for (const auto& icon : icons) if (name == icon.name) return icon.glyph;
    return nullptr;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[++i];
            out += next == 'n' ? '\n' : next;
        } else out += s[i];
    }
    return out;
}

// Replaces {icon-name} tokens; numeric {N} placeholders are left for tr().
std::string expand_icons(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '{') {
            size_t end = s.find('}', i);
            if (end != std::string::npos) {
                const char* glyph = icon_glyph(s.substr(i + 1, end - i - 1));
                if (glyph) { out += glyph; i = end; continue; }
            }
        }
        out += s[i];
    }
    return out;
}

bool read_file(const std::string& path, std::string& data) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    char buffer[4096];
    size_t count;
    data.clear();
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) data.append(buffer, count);
    std::fclose(file);
    if (data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);
    return true;
}

// Calls fn(key, value) for every "key = value" line; directives start with '@'.
template <class Fn>
void parse(const std::string& data, Fn&& fn) {
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        std::string line = trim(data.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string value = trim(line.substr(eq + 1));
        // "…" keeps leading/trailing spaces: key = " text "
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        if (!fn(trim(line.substr(0, eq)), value)) return;
    }
}

std::string file_path(const std::string& code) { return state().directory + "/" + code + ".lang"; }

std::shared_ptr<const Table> load_table(const std::string& file_code) {
    std::string data;
    auto table = std::make_shared<Table>();
    if (!read_file(file_path(file_code), data)) return table;
    parse(data, [&](const std::string& key, const std::string& value) {
        if (key[0] != '@') (*table)[key] = expand_icons(unescape(value));
        return true;
    });
    return table;
}

std::vector<std::string> list_lang_files(const std::string& directory) {
    std::vector<std::string> codes;
    auto add = [&](const std::string& name) {
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".lang") == 0)
            codes.push_back(name.substr(0, name.size() - 5));
    };
#ifdef __vita__
    SceUID dir = sceIoDopen(directory.c_str());
    if (dir >= 0) {
        SceIoDirent entry{};
        while (sceIoDread(dir, &entry) > 0) { add(entry.d_name); entry = {}; }
        sceIoDclose(dir);
    }
#else
    if (DIR* dir = opendir(directory.c_str())) {
        while (dirent* entry = readdir(dir)) add(entry->d_name);
        closedir(dir);
    }
#endif
    std::sort(codes.begin(), codes.end());
    return codes;
}

// Returns the registered language matching `code` exactly or by its base ("pt-BR" -> "pt").
const Language* find_language(const std::vector<Language>& languages, const std::string& code) {
    std::string wanted = lower(code);
    for (const auto& l : languages) if (lower(l.code) == wanted) return &l;
    auto base_of = [](const std::string& code) { return code.substr(0, code.find_first_of("-_")); };
    // "pt-BR" -> "pt", and "pt" -> the first "pt-*" language (so a console language still finds a regional file).
    for (const auto& l : languages) if (lower(l.code) == base_of(wanted)) return &l;
    for (const auto& l : languages) if (base_of(lower(l.code)) == base_of(wanted)) return &l;
    return nullptr;
}

// file name of the language (the code from the header may differ in case only).
std::string file_code_for(const std::string& directory, const std::string& code) {
    for (const auto& file : list_lang_files(directory)) if (lower(file) == lower(code)) return file;
    return code;
}

}  // namespace

std::string systemLanguage() {
#ifdef __vita__
    int value = 0;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &value) < 0) return "";
    switch (value) {
        case SCE_SYSTEM_PARAM_LANG_JAPANESE: return "ja";
        case SCE_SYSTEM_PARAM_LANG_ENGLISH_US: return "en";
        case SCE_SYSTEM_PARAM_LANG_FRENCH: return "fr";
        case SCE_SYSTEM_PARAM_LANG_SPANISH: return "es";
        case SCE_SYSTEM_PARAM_LANG_GERMAN: return "de";
        case SCE_SYSTEM_PARAM_LANG_ITALIAN: return "it";
        case SCE_SYSTEM_PARAM_LANG_DUTCH: return "nl";
        case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT: return "pt-PT";
        case SCE_SYSTEM_PARAM_LANG_RUSSIAN: return "ru";
        case SCE_SYSTEM_PARAM_LANG_KOREAN: return "ko";
        case SCE_SYSTEM_PARAM_LANG_CHINESE_T: return "zh-Hant";
        case SCE_SYSTEM_PARAM_LANG_CHINESE_S: return "zh-Hans";
        case SCE_SYSTEM_PARAM_LANG_FINNISH: return "fi";
        case SCE_SYSTEM_PARAM_LANG_SWEDISH: return "sv";
        case SCE_SYSTEM_PARAM_LANG_DANISH: return "da";
        case SCE_SYSTEM_PARAM_LANG_NORWEGIAN: return "no";
        case SCE_SYSTEM_PARAM_LANG_POLISH: return "pl";
        case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR: return "pt-BR";
        case SCE_SYSTEM_PARAM_LANG_ENGLISH_GB: return "en-GB";
        case SCE_SYSTEM_PARAM_LANG_TURKISH: return "tr";
        default: return "";
    }
#else
    return "";
#endif
}

void init(const std::string& directory, const std::string& preference) {
    State& s = state();
    std::vector<Language> found;
    for (const auto& file : list_lang_files(directory)) {
        Language language{file, file};
        std::string data;
        if (read_file(directory + "/" + file + ".lang", data)) {
            parse(data, [&](const std::string& key, const std::string& value) {
                if (key == "@code" && !value.empty()) language.code = value;
                else if (key == "@name" && !value.empty()) language.name = value;
                return key[0] == '@';  // directives come first
            });
        }
        found.push_back(language);
    }
    std::string english_file = file_code_for(directory, "en");
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.directory = directory;
        s.languages = found;
    }
    auto english = load_table(english_file);
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.english = english;
    }
    select(preference);
}

bool select(const std::string& preference) {
    State& s = state();
    std::vector<Language> languages;
    std::string directory;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        languages = s.languages;
        directory = s.directory;
    }
    const Language* chosen = nullptr;
    bool known = true;
    if (!preference.empty()) {
        chosen = find_language(languages, preference);
        known = chosen != nullptr;
    }
    if (!chosen && (preference.empty() || !known)) {
        std::string system = systemLanguage();
        if (!system.empty()) chosen = find_language(languages, system);
    }
    if (!chosen) chosen = find_language(languages, "en");
    std::string code = chosen ? chosen->code : "en";
    auto table = load_table(chosen ? file_code_for(directory, code) : "en");
    std::lock_guard<std::mutex> lock(s.mutex);
    s.current = code;
    s.active = table;
    return known;
}

const std::vector<Language>& languages() { return state().languages; }

std::string current() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.current;
}

std::string tr(const char* key) {
    State& s = state();
    std::shared_ptr<const Table> active, english;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        active = s.active;
        english = s.english;
    }
    auto it = active->find(key);
    if (it != active->end()) return it->second;
    it = english->find(key);
    return it != english->end() ? it->second : std::string(key);
}

std::string tr(const char* key, std::initializer_list<std::string> args) {
    std::string text = tr(key);
    std::string out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') {
            size_t end = i + 1;
            size_t index = 0;
            while (end < text.size() && std::isdigit((unsigned char)text[end])) index = index * 10 + (text[end++] - '0');
            if (end > i + 1 && end < text.size() && text[end] == '}') {
                if (index < args.size()) out += *(args.begin() + index);
                i = end;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

}  // namespace i18n
