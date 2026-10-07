#include "i18n.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#ifndef VITAGRAM_LANG_DIR
#error VITAGRAM_LANG_DIR must point to assets/lang
#endif

namespace {
std::map<std::string, std::string> read_entries(const std::string& path) {
    std::ifstream in(path);
    std::map<std::string, std::string> entries;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '@') continue;
        size_t eq = line.find(" = ");
        assert(eq != std::string::npos);
        entries[line.substr(0, eq)] = line.substr(eq + 3);
    }
    return entries;
}

// {0}, {1}... used by a text, as a sorted set.
std::set<std::string> placeholders(const std::string& text) {
    std::set<std::string> found;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '{') continue;
        size_t end = text.find('}', i);
        if (end == std::string::npos) break;
        std::string name = text.substr(i + 1, end - i - 1);
        if (!name.empty() && isdigit((unsigned char)name[0])) found.insert(name);
        i = end;
    }
    return found;
}

void write(const std::string& path, const std::string& text) { std::ofstream(path) << text; }
}  // namespace

int main() {
    const std::string dir = VITAGRAM_LANG_DIR;

    // Shipped catalogs: same keys, same placeholders, no raw icon glyphs, and every icon token is a known one.
    auto en = read_entries(dir + "/en.lang");
    auto ru = read_entries(dir + "/ru.lang");
    assert(!en.empty());
    for (const auto& entry : en) {
        assert(ru.count(entry.first) && "key missing in ru.lang");
        assert(placeholders(entry.second) == placeholders(ru[entry.first]) && "placeholders differ between languages");
    }
    for (const auto& entry : ru) assert(en.count(entry.first) && "key missing in en.lang");
    for (const auto* catalog : {&en, &ru})
        for (const auto& entry : *catalog) {
            for (size_t i = 0; i + 2 < entry.second.size(); ++i)  // U+E000..U+E0FF is EE 80..EE 83 xx
                assert(!((unsigned char)entry.second[i] == 0xEE && (unsigned char)entry.second[i + 1] >= 0x80 &&
                         (unsigned char)entry.second[i + 1] <= 0x83) && "icon glyph typed instead of {token}");
            std::string rest = entry.second;
            size_t open;
            while ((open = rest.find('{')) != std::string::npos) {
                size_t close = rest.find('}', open);
                assert(close != std::string::npos);
                std::string name = rest.substr(open + 1, close - open - 1);
                assert(isdigit((unsigned char)name[0]) || name == "cross" || name == "circle" || name == "square" ||
                       name == "triangle" || name == "dpad" || name == "start" || name == "select");
                rest.erase(0, close + 1);
            }
        }
    // The icon token for the same hint is substituted identically in every language.
    i18n::init(dir, "en");
    std::string english = tr("hint.back");
    i18n::init(dir, "ru");
    std::string russian = tr("hint.back");
    assert(english.find("\xEE\x80\x83") != std::string::npos && russian.find("\xEE\x80\x83") != std::string::npos);
    assert(english != russian);

    // Language list comes from the files; selection by code and unknown codes.
    assert(i18n::languages().size() >= 2);
    assert(i18n::select("ru") && i18n::current() == "ru");
    assert(i18n::tr("common.yes") == "Да");
    assert(i18n::select("en") && i18n::tr("common.yes") == "Yes");
    assert(!i18n::select("xx-unknown"));          // unknown -> automatic -> English off the console
    assert(i18n::current() == "en");
    assert(i18n::select("RU") && i18n::current() == "ru");   // case-insensitive

    // Arguments and fallbacks.
    assert(i18n::tr("sticker.page", {"2", "5"}) == "Страница 2 / 5");
    assert(i18n::tr("no.such.key") == "no.such.key");

    // A scratch language: missing keys fall back to English, quotes keep spaces, escapes work, BOM is ignored.
    char pattern[] = "/tmp/vitagram-i18n-XXXXXX";
    std::string scratch = mkdtemp(pattern);
    write(scratch + "/en.lang", "@code = en\n@name = English\nonly.en = English only\nspaced = \" a \"\nfmt = {1}-{0}\nlines = a\\nb\n");
    write(scratch + "/pt-BR.lang", "\xEF\xBB\xBF@code = pt-BR\n@name = Português\nspaced = \"[ b ]\"\n");
    i18n::init(scratch, "pt");                       // base language matches pt-BR
    assert(i18n::current() == "pt-BR");
    assert(i18n::tr("only.en") == "English only");   // fallback
    assert(i18n::tr("spaced") == "[ b ]");
    i18n::select("en");
    assert(i18n::tr("spaced") == " a ");
    assert(i18n::tr("fmt", {"x", "y"}) == "y-x");
    assert(i18n::tr("lines") == "a\nb");
    assert(i18n::languages().size() == 2 && i18n::languages()[1].name == "Português");
    unlink((scratch + "/en.lang").c_str());
    unlink((scratch + "/pt-BR.lang").c_str());
    rmdir(scratch.c_str());

    // Missing directory: keys are returned as-is, nothing crashes.
    i18n::init("/nonexistent-vitagram-lang", "");
    assert(i18n::tr("any.key") == "any.key" && i18n::languages().empty());
    std::puts("i18n: catalogs, icon tokens, selection, fallback, arguments and file format: PASS");
}
