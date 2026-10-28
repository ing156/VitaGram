#include "keyboard_layout.h"
#include <algorithm>
#include "i18n.h"
#include "utf8_decode.h"
#include "keyboard_languages.h"

namespace keyboard_layout {
const std::vector<Layout>& all() {
    static std::vector<Layout> layouts;
    static uint64_t generation = UINT64_MAX;
    if (generation == i18n::revision()) return layouts;
    generation = i18n::revision();
    layouts.clear();
    for (const auto& code : keyboard_languages::supported()) {
        const auto data = i18n::directives(code);
        auto value = [&](const std::string& key) {
            auto at = data.find("@keyboard." + key);
            return at == data.end() ? std::string() : at->second;
        };
        if (characters(value("badge")).empty() || characters(value("badge")).size() > 12) continue;
        Layout layout;
        layout.code = code; layout.badge = value("badge");
        for (const auto& language : i18n::languages()) if (language.code == code) layout.name = language.name;
        std::string rows = value("rows");
        if (!rows.empty()) {
            size_t begin = 0;
            for (;;) {
                size_t end = rows.find('|', begin);
                layout.rows.push_back(rows.substr(begin, end == std::string::npos ? end : end-begin));
                if (end == std::string::npos) break;
                begin = end+1;
            }
            if (layout.rows.size() != 3 || std::any_of(layout.rows.begin(), layout.rows.end(), [](const std::string& row) {
                const auto n = characters(row).size(); return n < 1 || n > 12;
            })) continue;
        } else if (code != "ja" && code != "ko" && code != "zh-Hans" && code != "zh-Hant") continue;
        const auto lower = characters(value("upper.lower")), caps = characters(value("upper.upper"));
        if (lower.size() != caps.size()) continue;
        for (size_t i=0; i<lower.size(); ++i) layout.uppercase[lower[i]] = caps[i];
        for (const auto& entry : data) if (entry.first.compare(0,16,"@keyboard.popup.") == 0) {
            auto options = characters(entry.second);
            if (!options.empty() && options.size() <= 11 && characters(entry.first.substr(16)).size() == 1) layout.popups[entry.first.substr(16)] = std::move(options);
        }
        for (size_t i=0; i<3; ++i) layout.symbols.push_back(value("symbols." + std::to_string(i)));
        if (std::any_of(layout.symbols.begin(), layout.symbols.end(), [](const std::string& row) {
            const auto n = characters(row).size(); return n < 1 || n > 14;
        })) continue;
        for (const auto& entry : data) if (entry.first.compare(0,16,"@keyboard.shift.") == 0 &&
            characters(entry.first.substr(16)).size() == 1 && characters(entry.second).size() == 1)
            layout.shifted[entry.first.substr(16)] = entry.second;
        layout.digits = characters(value("digits"));
        layout.digit_start = value("digit_start") == "1" ? 1 : 0;
        layouts.push_back(std::move(layout));
    }
    // Recovery layout keeps secret entry usable if all files are missing or invalid.
    if (layouts.empty() || layouts.front().code != "en")
        layouts.insert(layouts.begin(), Layout{"en", "EN", "English", {"qwertyuiop", "asdfghjkl", "zxcvbnm"}, {}, {}, {}, 0, {"1234567890", "@#$%&*()", ",.;:!?"}, {}});
    auto& codes = keyboard_languages::registry();
    codes.clear();
    for (const auto& layout : layouts) codes.push_back(layout.code);
    return layouts;
}
const Layout& get(const std::string& code) {
    for (const auto& layout : all()) if (layout.code == code) return layout;
    return all().front();
}
std::vector<std::string> characters(const std::string& text) {
    std::vector<std::string> result;
    for (size_t i = 0; i < text.size();) {
        uint32_t scalar = 0;
        const size_t n = decode_utf8_scalar(text.c_str()+i, scalar);
        if (scalar < 0x20 || scalar == 0x7f || (scalar == 0xfffd && n == 1)) return {};
        result.push_back(text.substr(i,n)); i += n;
    }
    return result;
}
std::string upper(const std::string& text, const std::string& code) {
    const auto& pairs = get(code).uppercase;
    std::string result;
    for (const auto& c : characters(text)) {
        const auto found = pairs.find(c);
        if (found != pairs.end()) result += found->second;
        else if (c.size() == 1 && c[0] >= 'a' && c[0] <= 'z') result += static_cast<char>(c[0] - 'a' + 'A');
        else result += c;
    }
    return result;
}
std::vector<std::string> alternatives(const Layout& layout, const std::string& label,
                                     size_t row, size_t column, bool shifted) {
    std::vector<std::string> result;
    const auto found = layout.popups.find(label);
    if (found != layout.popups.end())
        for (const auto& option : found->second) result.push_back(shifted ? upper(option, layout.code) : option);
    if (row == 0 && column >= layout.digit_start && column-layout.digit_start < layout.digits.size()) {
        const auto& digit = layout.digits[column-layout.digit_start];
        if (std::find(result.begin(), result.end(), digit) == result.end()) result.push_back(digit);
    }
    return result;
}
}
