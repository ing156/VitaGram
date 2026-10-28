#include "i18n.h"
#include "keyboard_languages.h"
#include <algorithm>
#include <atomic>
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
std::atomic<uint64_t> catalog_revision{0};

using Table = std::unordered_map<std::string, std::string>;

struct State {
    std::mutex mutex;
    std::string directory;
    std::vector<Language> languages;
    std::map<std::string, std::string> paths;
    std::map<std::string, std::map<std::string, std::string>> metadata;
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
        {"dpad", ""}, {"start", ""}, {"select", ""}, {"l", ""}, {"r", ""},
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
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (data.size() + count > 512 * 1024) { std::fclose(file); data.clear(); return false; }
        data.append(buffer, count);
    }
    const bool failed = std::ferror(file);
    std::fclose(file);
    if (failed) { data.clear(); return false; }
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
        if (eq == std::string::npos || trim(line.substr(0, eq)).empty()) continue;
        std::string value = trim(line.substr(eq + 1));
        // "…" keeps leading/trailing spaces: key = " text "
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        if (!fn(trim(line.substr(0, eq)), value)) return;
    }
}

std::string file_path(const std::string& code) {
    const auto at = state().paths.find(code);
    return at == state().paths.end() ? state().directory + "/" + code + ".lang" : at->second;
}

std::shared_ptr<const Table> load_table(const std::string& file_code) {
    std::string data;
    auto table = std::make_shared<Table>();
    const std::string base = state().directory + "/" + file_code + ".lang";
    const std::string selected = file_path(file_code);
    for (const auto& path : {base, selected}) {
        if (!read_file(path, data)) continue;
        parse(data, [&](const std::string& key, const std::string& value) {
            if (key[0] != '@') (*table)[key] = expand_icons(unescape(value));
            return true;
        });
        if (base == selected) break;
    }
    return table;
}

std::vector<std::string> list_lang_files(const std::string& directory) {
    std::vector<std::string> codes;
    auto add = [&](const std::string& name) {
        if (codes.size() < 256 && name.size() > 5 && name.compare(name.size() - 5, 5, ".lang") == 0)
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

void init(const std::string& directory, const std::string& preference, const std::string& custom_directory) {
    State& s = state();
    std::vector<Language> found;
    std::map<std::string, std::string> paths;
    std::map<std::string, std::map<std::string, std::string>> metadata;
    for (const auto& folder : {directory, custom_directory}) {
        if (folder.empty()) continue;
        for (const auto& file : list_lang_files(folder)) {
            if (file.size() > 64 || !std::all_of(file.begin(), file.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '-' || c == '_';
            })) continue;
            if (found.size() >= 64 && paths.find(file) == paths.end()) continue;
            Language language{file, file};
            std::string data;
            if (!read_file(folder + "/" + file + ".lang", data)) continue;
            std::map<std::string, std::string> headers;
            parse(data, [&](const std::string& key, const std::string& value) {
                if (key[0] == '@' && key.size() <= 80 && value.size() <= 4096 && headers.size() < 128) headers[key] = unescape(value);
                return true;
            });
            // File identity is authoritative: headers cannot redirect reads to another path.
            if (headers.count("@code") && lower(headers["@code"]) != lower(file)) continue;
            if (!headers["@name"].empty()) language.name = headers["@name"];
            auto at = std::find_if(found.begin(), found.end(), [&](const Language& item) { return lower(item.code) == lower(file); });
            if (at == found.end()) found.push_back(language);
            else { language.code = at->code; *at = language; }
            paths[language.code] = folder + "/" + file + ".lang";
            for (auto& header : headers) metadata[language.code][header.first] = std::move(header.second);
        }
    }
    auto& keyboard_codes = keyboard_languages::registry();
    keyboard_codes = keyboard_languages::native();
    for (const auto& language : found)
        if (!keyboard_languages::contains(keyboard_codes, language.code) &&
            !metadata[language.code]["@keyboard.rows"].empty()) keyboard_codes.push_back(language.code);
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.directory = directory; s.languages = std::move(found);
        s.paths = std::move(paths); s.metadata = std::move(metadata);
    }
    auto english = load_table("en");
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.english = english;
    }
    select(preference);
}

std::map<std::string, std::string> directives(const std::string& code) {
    std::lock_guard<std::mutex> lock(state().mutex);
    const auto at = state().metadata.find(code);
    return at == state().metadata.end() ? std::map<std::string, std::string>{} : at->second;
}

std::string resolve(const std::string& preference) {
    std::vector<Language> languages;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        languages = state().languages;
    }
    const Language* chosen = nullptr;
    if (!preference.empty()) {
        chosen = find_language(languages, preference);
    }
    if (!chosen) {
        std::string system = systemLanguage();
        if (!system.empty()) chosen = find_language(languages, system);
    }
    if (!chosen) chosen = find_language(languages, "en");
    return chosen ? chosen->code : "en";
}

bool select(const std::string& preference) {
    State& s = state();
    std::vector<Language> languages;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        languages = s.languages;
    }
    const bool known = preference.empty() || find_language(languages, preference);
    const std::string code = resolve(preference);
    auto table = load_table(code);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.current = code;
    s.active = table;
    ++catalog_revision;
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

uint64_t revision() { return catalog_revision.load(); }
Text Text::computed(std::function<std::string()> render) {
    Text result;
    result.render_ = std::make_shared<const std::function<std::string()>>(std::move(render));
    result.revision_ = revision() - 1;
    result.refresh();
    return result;
}
std::string Text::localized() const {
    if (!render_ || revision_ == revision()) return static_cast<const std::string&>(*this);
    Text updated = *this;
    updated.refresh();
    return static_cast<const std::string&>(updated);
}
void Text::refresh() {
    if (!render_) { revision_ = revision(); return; }
    for (;;) {
        const uint64_t generation = revision();
        if (revision_ == generation) return;
        std::string value = (*render_)();
        // A media/storage worker may finish while the UI commits another locale.
        // Do not mark an older rendering as belonging to the new catalog.
        if (generation != revision()) continue;
        std::string::operator=(std::move(value));
        revision_ = generation;
        return;
    }
}
Text text(const char* key, std::initializer_list<Text> args) {
    return Text::computed([key = std::string(key), args = std::vector<Text>(args)] {
        const std::string pattern = tr(key.c_str());
        std::string out;
        for (size_t i = 0; i < pattern.size(); ++i) {
            if (pattern[i] == '{') {
                size_t end = i + 1, index = 0;
                while (end < pattern.size() && std::isdigit(static_cast<unsigned char>(pattern[end])))
                    index = index * 10 + (pattern[end++] - '0');
                if (end > i + 1 && end < pattern.size() && pattern[end] == '}') {
                    if (index < args.size()) out += args[index].localized();
                    i = end; continue;
                }
            }
            out += pattern[i];
        }
        return out;
    });
}
Text operator+(const Text& left, const Text& right) {
    if (!left.render_ && !right.render_)
        return Text(static_cast<const std::string&>(left) + static_cast<const std::string&>(right));
    return Text::computed([left, right] { return left.localized() + right.localized(); });
}
Text operator+(const Text& left, const std::string& right) { return left + Text(right); }
Text operator+(const std::string& left, const Text& right) { return Text(left) + right; }
Text operator+(const Text& left, std::string&& right) { return left + Text(std::move(right)); }
Text operator+(std::string&& left, const Text& right) { return Text(std::move(left)) + right; }
Text operator+(const Text& left, const char* right) { return left + Text(right); }
Text operator+(const char* left, const Text& right) { return Text(left) + right; }
Text& Text::operator+=(const Text& value) { return *this = *this + value; }
void Text::replaceTranslation(const Text& needle, const Text& replacement) {
    *this = computed([original = *this, needle, replacement] {
        std::string value = original.localized();
        const std::string from = needle.localized();
        const auto position = value.find(from);
        if (position != std::string::npos) value.replace(position, from.size(), replacement.localized());
        return value;
    });
}

}  // namespace i18n
