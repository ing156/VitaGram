#pragma once
#include <cstdint>
#include <string>
#include <vector>

// TDLib minithumbnails are small JPEGs already present in the response. Keep
// them in RAM, including when the account's files are encrypted.
namespace inline_thumbnail {
constexpr const char* prefix = "inline-jpeg:";
constexpr size_t max_encoded = 16 * 1024;
inline bool decode(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    if (path.compare(0, 12, prefix) || path.size() <= 12 || path.size() > 12 + max_encoded) return false;
    const std::string text = path.substr(12);
    if (text.size() % 4) return false;
    auto digit = [](char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    for (size_t i = 0; i < text.size(); i += 4) {
        int a = digit(text[i]), b = digit(text[i + 1]);
        int c = text[i + 2] == '=' ? 0 : digit(text[i + 2]);
        int d = text[i + 3] == '=' ? 0 : digit(text[i + 3]);
        const bool pad2 = text[i + 2] == '=', pad1 = text[i + 3] == '=';
        if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad1) ||
            ((pad1 || pad2) && i + 4 != text.size()) || (pad2 && (b & 15)) || (pad1 && !pad2 && (c & 3))) {
            out.clear(); return false;
        }
        out.push_back((a << 2) | (b >> 4));
        if (!pad2) out.push_back((b << 4) | (c >> 2));
        if (!pad1) out.push_back((c << 6) | d);
    }
    if (out.size() < 3 || out[0] != 0xff || out[1] != 0xd8 || out[2] != 0xff) { out.clear(); return false; }
    return true;
}
inline std::string key(const char* encoded) {
    if (!encoded) return "";
    std::string result = std::string(prefix) + encoded;
    std::vector<uint8_t> bytes;
    return decode(result, bytes) ? result : "";
}
}
