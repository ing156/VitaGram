#include "vpk_package.h"
#include <algorithm>
#include <cstring>
#include <openssl/sha.h>

namespace vpk {
namespace {
uint32_t le(const uint8_t* p) { return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint32_t be(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
bool title_id(const std::string& id) {
    return id.size() == 9 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
}
}
bool filename(const std::string& name) {
    if (name.size() < 5) return false;
    std::string suffix = name.substr(name.size() - 4);
    for (char& c : suffix) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return suffix == ".vpk";
}
bool parse_sfo(const std::vector<uint8_t>& b, Metadata& m) {
    m = {};
    if (b.size() < 20 || b.size() > 1024 * 1024 || le(b.data()) != 0x46535000) return false;
    const size_t keys = le(&b[8]), data = le(&b[12]), count = le(&b[16]);
    if (count > (b.size() - 20) / 16 || keys < 20 + count * 16 || data < keys || data >= b.size()) return false;
    std::string category;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* e = &b[20 + 16 * i];
        size_t key = keys + (e[0] | unsigned(e[1]) << 8);
        size_t offset = le(e + 12), length = le(e + 4), capacity = le(e + 8);
        if (key >= data || !std::memchr(&b[key], 0, data - key) || offset > b.size() - data ||
            length > capacity || capacity > b.size() - data - offset) return false;
        std::string name(reinterpret_cast<const char*>(&b[key]));
        if (name != "TITLE_ID" && name != "TITLE" && name != "CONTENT_ID" && name != "CATEGORY") continue;
        if (e[2] != 4 || e[3] != 2 || !length || b[data + offset + length - 1] != 0) return false;
        std::string value(reinterpret_cast<const char*>(&b[data + offset]), length - 1);
        if (value.find('\0') != std::string::npos) return false;
        std::string* target = name == "TITLE_ID" ? &m.title_id : name == "TITLE" ? &m.title : name == "CONTENT_ID" ? &m.content_id : &category;
        if (!target->empty()) return false;
        *target = value;
    }
    if (!title_id(m.title_id) || category != "gd" || m.title.empty() || m.title.size() > 256) return false;
    if (!m.content_id.empty() && (m.content_id.size() != 36 || m.content_id.substr(7, 9) != m.title_id ||
        !std::all_of(m.content_id.begin(), m.content_id.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        }))) return false;
    return true;
}
bool make_head(std::vector<uint8_t>& h, const Metadata& m) {
    if (!title_id(m.title_id) || h.size() < 0x100 || h.size() > 1024 * 1024) return false;
    const size_t header = be(&h[0xd0]), info = be(&h[8]), size = be(&h[0x10]), output = be(&h[0xd4]), all = be(&h[0xe8]);
    auto range = [&](size_t start, size_t n) { return start <= h.size() && n <= h.size() - start; };
    if (header < 0x60 || size < 64 || !range(0, header) || !range(header, 16) ||
        !range(info, size - 64) || !range(output, 16) || !range(0, all) || !range(all, 16)) return false;
    std::string content = m.content_id.empty() ? "EP9000-" + m.title_id + "_00-0000000000000000" : m.content_id;
    if (content.size() != 36) return false;
    std::fill(h.begin() + 0x30, h.begin() + 0x60, 0);
    std::copy(content.begin(), content.end(), h.begin() + 0x30);
    // Homebrew fake-package digest format (two SHA-1 passes, 16-byte output).
    auto digest = [&](size_t start, size_t length, size_t dest) {
        unsigned char first[SHA_DIGEST_LENGTH], block[64]{}, result[SHA_DIGEST_LENGTH];
        SHA1(h.data() + start, length, first);
        for (int i = 0; i < 16; ++i) block[i] = first[4 + i % 8];
        std::copy(first + 12, first + 17, block + 16);
        std::copy(first + 1, first + 4, block + 21);
        std::copy(block + 16, block + 24, block + 24);
        SHA1(block, sizeof(block), result);
        std::copy(result, result + 16, h.begin() + dest);
    };
    digest(0, header, header);
    digest(info, size - 64, output);
    digest(0, all, all);
    return true;
}
}
