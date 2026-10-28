#include "i18n.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <vector>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

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
    i18n::init(dir, "en");
    auto en = read_entries(dir + "/en.lang");
    assert(!en.empty());
    std::vector<std::map<std::string, std::string>> catalogs{en};
    std::vector<std::string> names{"en"};
    for (const auto& language : i18n::languages()) {
        if (language.code == "en") continue;
        catalogs.push_back(read_entries(dir + "/" + language.code + ".lang"));
        names.push_back(language.code);
    }
    for (size_t c = 1; c < catalogs.size(); ++c) {
        for (const auto& entry : en) {
            auto it = catalogs[c].find(entry.first);
            if (it == catalogs[c].end()) { std::fprintf(stderr, "%s.lang: missing key %s\n", names[c].c_str(), entry.first.c_str()); return 1; }
            if (placeholders(entry.second) != placeholders(it->second)) {
                std::fprintf(stderr, "%s.lang: placeholders differ for %s\n", names[c].c_str(), entry.first.c_str()); return 1;
            }
        }
        for (const auto& entry : catalogs[c])
            if (!en.count(entry.first)) { std::fprintf(stderr, "%s.lang: unknown key %s\n", names[c].c_str(), entry.first.c_str()); return 1; }
    }
    for (const auto& catalog : catalogs)
        for (const auto& entry : catalog) {
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
                       name == "triangle" || name == "dpad" || name == "start" || name == "select" || name == "l" || name == "r");
                rest.erase(0, close + 1);
            }
        }
    // Icon tokens in a hint must be identical in every language (only the text may differ).
    i18n::init(dir, "en");
    auto icons = [](const std::string& s) { std::string r; for (size_t i = 0; i + 2 < s.size(); ++i) if ((unsigned char)s[i] == 0xEE) r += s.substr(i, 3); return r; };
    for (const auto& language : i18n::languages()) {
        i18n::select(language.code);
        for (const auto& entry : en) {
            std::string localized = tr(entry.first.c_str());
            i18n::select("en");
            std::string reference = tr(entry.first.c_str());
            i18n::select(language.code);
            if (icons(reference) != icons(localized)) {
                std::fprintf(stderr, "%s.lang: icons differ for %s\n", language.code.c_str(), entry.first.c_str()); return 1;
            }
        }
    }
    // The icon token for the same hint is substituted identically in every language.
    i18n::init(dir, "en");
    assert(tr("menu.rotate_left").find("\xEE\x80\x87") != std::string::npos);
    assert(tr("menu.rotate_right").find("\xEE\x80\x88") != std::string::npos);
    std::string english = tr("hint.back");
    i18n::init(dir, "ru");
    std::string russian = tr("hint.back");
    assert(english.find("\xEE\x80\x83") != std::string::npos && russian.find("\xEE\x80\x83") != std::string::npos);
    assert(english != russian);

    // Language list comes from the files; selection by code and unknown codes.
    assert(i18n::languages().size() >= 2);
    assert(i18n::select("ru") && i18n::current() == "ru");
    assert(i18n::resolve("zh-Hans") == "zh-Hans");
    assert(i18n::resolve("KO") == "ko");
    assert(i18n::resolve("xx-unknown") == "en");
    assert(i18n::current() == "ru");
    assert(i18n::tr("common.yes") == "Да");
    assert(i18n::select("en") && i18n::tr("common.yes") == "Yes");
    assert(!i18n::select("xx-unknown"));          // unknown -> automatic -> English off the console
    assert(i18n::current() == "en");
    assert(i18n::select("RU") && i18n::current() == "ru");   // case-insensitive

    // Arguments and fallbacks.
    assert(i18n::tr("sticker.page", {"2", "5"}) == "Страница 2 / 5");
    assert(i18n::tr("no.such.key") == "no.such.key");

    // Retained UI strings carry keys, nested arguments and composition across changes.
    i18n::Text retained = i18n::text("doc.downloading", {i18n::text("unit.kib", {"17"})});
    i18n::Text camera = i18n::text("cam.open_failed") + " (0x1234)";
    i18n::Text album = i18n::text("media.photo") + " · GIF";
    album.replaceTranslation(i18n::text("media.photo"), i18n::text("media.photos_count", {"3"}));
    i18n::Text literal = tr("status.online"); // Same bytes, but explicitly raw user/server content.
    const std::string unchanged = literal;
    const auto generation = i18n::revision();
    assert(i18n::select("en") && i18n::revision() > generation);
    retained.refresh(); camera.refresh(); album.refresh(); literal.refresh();
    assert(retained == tr("doc.downloading", {tr("unit.kib", {"17"})}));
    assert(camera == tr("cam.open_failed") + " (0x1234)");
    assert(album == tr("media.photos_count", {"3"}) + " · GIF");
    assert(literal == unchanged);
    i18n::Text copy = retained;
    retained.clear(); // Clearing must drop the recipe instead of reviving old status text.
    camera = "SERVER_ERROR"; // Raw replacement must also drop the old key.
    assert(i18n::select("ru"));
    retained.refresh(); camera.refresh(); copy.refresh();
    assert(retained.empty() && camera == "SERVER_ERROR");
    assert(copy == tr("doc.downloading", {tr("unit.kib", {"17"})}));

    // A worker that renders during a language commit must not cache the old text
    // with the new generation. Block inside the real renderer deterministically.
    std::mutex mutex;
    std::condition_variable changed;
    bool captured = false, resume = false;
    std::atomic<bool> first{true};
    i18n::Text worker_status;
    std::thread worker([&] {
        worker_status = i18n::Text::computed([&] {
            const std::string value = tr("status.online");
            if (first.exchange(false)) {
                std::unique_lock<std::mutex> lock(mutex);
                captured = true; changed.notify_all();
                changed.wait(lock, [&] { return resume; });
            }
            return value;
        });
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(10), [&] { return captured; }));
    }
    assert(i18n::select("en"));
    { std::lock_guard<std::mutex> lock(mutex); resume = true; }
    changed.notify_all(); worker.join();
    assert(worker_status == tr("status.online"));
    assert(i18n::select("ru"));

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
    const auto custom = scratch + "/custom";
    assert(mkdir(custom.c_str(), 0700) == 0);
    write(custom + "/xx.lang", "@code = xx\n@name = Custom\nspaced = Custom text\n");
    write(custom + "/pt-BR.lang", "@code = pt-BR\n@name = Override\nspaced = Override text\n");
    write(custom + "/bad.lang", "@code = ../en\n@name = Invalid\n");
    i18n::init(scratch, "xx", custom);
    assert(i18n::current() == "xx" && tr("spaced") == "Custom text");
    assert(tr("only.en") == "English only");
    assert(i18n::languages().size() == 3);
    assert(i18n::select("pt-BR") && tr("spaced") == "Override text");
    unlink((custom + "/xx.lang").c_str());
    unlink((custom + "/pt-BR.lang").c_str());
    unlink((custom + "/bad.lang").c_str());
    rmdir(custom.c_str());
    unlink((scratch + "/en.lang").c_str());
    unlink((scratch + "/pt-BR.lang").c_str());
    rmdir(scratch.c_str());

    // Missing directory: keys are returned as-is, nothing crashes.
    i18n::init("/nonexistent-vitagram-lang", "");
    assert(i18n::tr("any.key") == "any.key" && i18n::languages().empty());
    std::puts("i18n: catalogs, icon tokens, selection, fallback, arguments and file format: PASS");
}
