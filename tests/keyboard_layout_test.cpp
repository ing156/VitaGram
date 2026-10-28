#include "keyboard_layout.h"
#include "i18n.h"
#include "keyboard_languages.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <set>
#include <fstream>
#include <unistd.h>

int main(int argc, char** argv) {
    using namespace keyboard_layout;
    assert(argc == 2);
    i18n::init(argv[1], "en");
    assert(characters("\xC0\xAF").empty());
    assert(characters("\xED\xA0\x80").empty());
    assert(characters("\xF4\x90\x80\x80").empty());
    assert(characters("a\nb").empty());
    std::set<std::string> codes;
    int native = 0;
    for (const auto& layout : all()) {
        assert(codes.insert(layout.code).second);
        assert(std::filesystem::exists(std::filesystem::path(argv[1]) / (layout.code + ".lang")));
        if (layout.system()) { ++native; continue; }
        assert(layout.rows.size() == 3);
        std::set<std::string> letters;
        for (size_t r = 0; r < layout.rows.size(); ++r) {
            const auto row = characters(layout.rows[r]);
            assert(row.size() >= 6 && row.size() <= 12);
            for (size_t k = 0; k < row.size(); ++k) {
                assert(letters.insert(row[k]).second);
                const auto lower = alternatives(layout, row[k], r, k, false);
                const auto caps = alternatives(layout, row[k], r, k, true);
                assert(lower.size() == caps.size() && lower.size()*72 + 12 <= 944);
                for (size_t i = 0; i < lower.size(); ++i) {
                    assert(caps[i] == upper(lower[i], layout.code));
                    assert(upper(caps[i], layout.code) == caps[i]);
                    assert(!lower[i].empty());
                }
            }
        }
        for (const auto& popup : layout.popups) {
            assert(letters.count(popup.first) || popup.first == "'" || popup.first == "\"" || popup.first == "," || popup.first == ".");
            assert(!popup.second.empty());
        }
    }
    assert(codes.size() == 19 && native == 4);
    assert(all().size() == keyboard_languages::supported().size());
    for (size_t i = 0; i < all().size(); ++i) assert(all()[i].code == keyboard_languages::supported()[i]);
    using List = std::vector<std::string>;
    assert(keyboard_languages::defaults("ru") == List({"en", "ru"}));
    assert(keyboard_languages::defaults("ja") == List({"en", "ja"}));
    for (const auto& system : {"en", "en-GB", "", "unsupported"}) assert(keyboard_languages::defaults(system) == List({"en"}));
    assert(keyboard_languages::normalize({"ru", "en-GB", "ru", "unknown"}) == List({"ru", "en"}));
    List saved;
    assert(keyboard_languages::resolve(saved, "fr") == List({"en", "fr"}));
    assert(keyboard_languages::toggle(saved, "en", "fr") && saved == List({"fr"}));
    assert(!keyboard_languages::toggle(saved, "fr", "fr") && saved == List({"fr"}));
    assert(keyboard_languages::toggle(saved, "ru", "fr") && saved == List({"fr", "ru"}));
    assert(!keyboard_languages::toggle(saved, "missing", "fr"));
    assert(keyboard_languages::resolve(saved, "ja") == List({"fr", "ru"}));
    int catalogs = 0;
    for (const auto& entry : std::filesystem::directory_iterator(argv[1]))
        if (entry.path().extension() == ".lang") { assert(codes.count(entry.path().stem().string())); ++catalogs; }
    assert(catalogs == 19);
    auto contains = [](const std::string& code, const std::string& key, const std::string& value) {
        const auto& choices = get(code).popups.at(key);
        assert(std::find(choices.begin(), choices.end(), value) != choices.end());
    };
    contains("de", "s", "ß"); contains("fr", "o", "œ"); contains("fr", "e", "ë");
    contains("es", "n", "ñ"); contains("it", "e", "è"); contains("nl", "i", "ĳ");
    contains("pt-BR", "a", "ã"); contains("pt-PT", "o", "õ");
    contains("sv", "a", "å"); contains("fi", "s", "š"); contains("da", "o", "ø");
    contains("no", "a", "æ"); contains("pl", "l", "ł"); contains("pl", "z", "ź");
    contains("tr", "i", "ı"); contains("tr", "g", "ğ");
    assert(upper("iıîşğçöü", "tr") == "İIÎŞĞÇÖÜ");
    assert(upper("ßœæĳ", "de") == "ẞŒÆĲ");
    assert(get("fr").rows[0] == "azertyuiop");
    assert(get("de").rows[0] == "qwertzuiop");
    assert(get("unknown").code == "en");
    char pattern[] = "/tmp/vitagram-keyboard-lang-XXXXXX";
    const std::string custom = mkdtemp(pattern);
    std::ifstream source(std::string(argv[1]) + "/en.lang");
    std::string catalog((std::istreambuf_iterator<char>(source)), {});
    catalog.replace(catalog.find("@code = en"), 10, "@code = xx");
    std::ofstream(custom + "/xx.lang") << catalog << "\n@keyboard.rows = abcdef|ghijkl|mnopqr\n@keyboard.upper.lower = abc\n@keyboard.upper.upper = XYZ\n";
    std::ofstream(custom + "/bad.lang") << catalog << "\n@code = bad\n@keyboard.rows = abc|def\n";
    i18n::init(argv[1], "xx", custom);
    assert(i18n::current() == "xx");
    assert(keyboard_languages::normalize({"xx"}) == List({"xx"}));
    assert(get("xx").rows[0] == "abcdef" && upper("abc", "xx") == "XYZ");
    assert(get("bad").code == "en");
    assert(get("ru").rows[0] == "йцукенгшщзх");
    std::filesystem::remove_all(custom);
    std::puts("keyboard layouts: all 19 catalogs, alphabets, locale alternatives, Turkish case, native routes and popup bounds: PASS");
}
