#pragma once
#include <cstdint>
// Decode one Unicode scalar. Invalid/truncated input consumes one byte.
inline int decode_utf8_scalar(const char* text, uint32_t& code) {
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    code = 0xfffd;
    if (!p[0]) { code = 0; return 0; }
    if (p[0] < 0x80) { code = p[0]; return 1; }
    int count = p[0] >= 0xc2 && p[0] <= 0xdf ? 2 :
        p[0] >= 0xe0 && p[0] <= 0xef ? 3 : p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0;
    if (!count) return 1;
    uint32_t value = p[0] & ((1u << (7-count)) - 1);
    for (int i=1; i<count; ++i) {
        if ((p[i] & 0xc0) != 0x80) return 1;
        value = (value << 6) | (p[i] & 0x3f);
    }
    if ((count == 2 && value < 0x80) || (count == 3 && value < 0x800) ||
        (count == 4 && value < 0x10000) || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) return 1;
    code = value;
    return count;
}
extern "C" int __wrap_utf8_to_ucs2(const char* text, unsigned int* code);
