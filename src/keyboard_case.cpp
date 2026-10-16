#include "keyboard_case.h"
#include <vector>
#include <algorithm>

namespace {
std::vector<uint32_t> decode(const std::string& text) {
    std::vector<uint32_t> result;
    for (size_t i = 0; i < text.size();) {
        const unsigned char first = text[i++];
        uint32_t cp = first;
        const int tail = first < 0x80 ? 0 : (first & 0xe0) == 0xc0 ? 1 :
            (first & 0xf0) == 0xe0 ? 2 : (first & 0xf8) == 0xf0 ? 3 : 0;
        if (tail) {
            cp = first & ((1u << (6-tail)) - 1);
            for (int k = 0; k < tail && i < text.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 63);
        }
        result.push_back(cp);
    }
    return result;
}
bool space(uint32_t c) {
    return (c >= 9 && c <= 13) || c == 32 || c == 0x85 || c == 0xa0 || c == 0x1680 ||
        (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}
bool lineBreak(uint32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }
bool quote(uint32_t c) {
    switch (c) {
        case '\'': case '"': case '(': case ')': case '[': case ']': case '{': case '}':
        case 0xab: case 0xbb: case 0xbf: case 0xa1: case 0x201a: case 0x201e: case 0x2018: case 0x2019: case 0x201c: case 0x201d:
        case 0x2039: case 0x203a: return true;
        default: return false;
    }
}
uint32_t lower(uint32_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 0x410 && c <= 0x42f)) return c + 32;
    if (c == 0x401) return 0x451;
    return c;
}
bool abbreviation(const std::vector<uint32_t>& text, size_t period) {
    size_t start = period;
    while (start && !space(text[start-1]) && !quote(text[start-1])) --start;
    std::vector<uint32_t> token(text.begin()+start, text.begin()+period);
    std::transform(token.begin(), token.end(), token.begin(), lower);
    // Conservative list of common titles/address markers and dotted abbreviations.
    // Explicit newlines always start a new sentence, even after an abbreviation.
    static const std::vector<std::vector<uint32_t>> words = [] {
        std::vector<std::vector<uint32_t>> result;
        for (const char* word : {"mr", "mrs", "ms", "dr", "prof", "sr", "sra", "srta", "mme", "mlle",
             "e.g", "i.e", "z.b", "d.h", "u.a", "np", "m.in", "г", "ул", "д", "стр", "рис", "им",
             "т", "е", "т.е", "т.д", "т.п", "т.н", "т.к", "т.о", "кв", "корп", "обл", "тел"}) result.push_back(decode(word));
        return result;
    }();
    return std::find(words.begin(), words.end(), token) != words.end();
}
}

bool KeyboardCase::sentenceStart(const std::string& value) {
    const auto text = decode(value);
    bool separated = false;
    size_t end = text.size();
    while (end) {
        const uint32_t c = text[end-1];
        if (lineBreak(c)) return true;
        if (space(c)) { separated = true; --end; continue; }
        if (quote(c)) { --end; continue; }
        if (c == 0x3002 || c == 0xff01 || c == 0xff1f) return true;
        if (!separated) return false;
        if (c == '.') return !abbreviation(text, end-1);
        return c == '!' || c == '?' || c == 0x2026;
    }
    return true;
}
