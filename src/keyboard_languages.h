#pragma once
#include <algorithm>
#include <string>
#include <vector>

// Shared preference rules for settings, the in-app picker and the native IME.
namespace keyboard_languages {
inline const std::vector<std::string>& supported() {
    static const std::vector<std::string> codes = {"en", "ru", "de", "fr", "es", "it", "nl", "pt-PT", "pt-BR",
        "sv", "da", "no", "fi", "pl", "tr", "ja", "ko", "zh-Hans", "zh-Hant"};
    return codes;
}
inline std::string canonical(const std::string& code) { return code == "en-GB" ? "en" : code; }
inline bool contains(const std::vector<std::string>& codes, const std::string& code) {
    return std::find(codes.begin(), codes.end(), code) != codes.end();
}
inline std::vector<std::string> normalize(const std::vector<std::string>& codes) {
    std::vector<std::string> result;
    for (const auto& value : codes) {
        const auto code = canonical(value);
        if (contains(supported(), code) && !contains(result, code)) result.push_back(code);
    }
    return result;
}
inline std::vector<std::string> defaults(const std::string& system) {
    return normalize({"en", system});
}
// An empty stored list means automatic, so old settings acquire the new defaults.
inline std::vector<std::string> resolve(const std::vector<std::string>& saved, const std::string& system) {
    if (saved.empty()) return defaults(system);
    auto result = normalize(saved);
    return result.empty() ? std::vector<std::string>{"en"} : result;
}
inline bool toggle(std::vector<std::string>& saved, const std::string& code, const std::string& system) {
    if (!contains(supported(), code)) return false;
    auto result = resolve(saved, system);
    const auto at = std::find(result.begin(), result.end(), code);
    if (at == result.end()) result.push_back(code);
    else if (result.size() > 1) result.erase(at);
    else return false; // Keep at least one usable input language.
    saved = std::move(result);
    return true;
}
}
