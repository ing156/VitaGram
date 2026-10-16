#include "vpk_package.h"
#include <openssl/sha.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>

static void le(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + i] = v >> (8 * i);
}
static void be(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + i] = v >> (8 * (3 - i));
}
static std::vector<uint8_t> sfo(std::map<std::string, std::string> fields) {
    size_t count = fields.size(), keys = 20 + count * 16, data = keys;
    for (auto& field : fields) data += field.first.size() + 1;
    std::vector<uint8_t> b(data);
    le(b, 0, 0x46535000); le(b, 4, 0x101); le(b, 8, keys); le(b, 12, data); le(b, 16, count);
    size_t entry = 20, key = keys;
    for (auto& field : fields) {
        size_t size = field.second.size() + 1;
        le(b, entry, uint32_t(key - keys) | 0x02040000); le(b, entry + 4, size);
        le(b, entry + 8, size); le(b, entry + 12, b.size() - data);
        std::memcpy(&b[key], field.first.c_str(), field.first.size() + 1);
        key += field.first.size() + 1; entry += 16;
        b.insert(b.end(), field.second.begin(), field.second.end()); b.push_back(0);
    }
    return b;
}
int main() {
    assert(vpk::filename("App.VPK") && vpk::filename("a.vpk"));
    assert(!vpk::filename(".vpk") && !vpk::filename("a.vpk.exe"));
    std::map<std::string, std::string> fields{{"TITLE_ID", "TESTAPP01"}, {"TITLE", "Test app"}, {"CATEGORY", "gd"}};
    auto valid = sfo(fields);
    vpk::Metadata metadata;
    assert(vpk::parse_sfo(valid, metadata) && metadata.title_id == "TESTAPP01");
    for (size_t n = 0; n < valid.size(); ++n) {
        auto truncated = valid; truncated.resize(n);
        assert(!vpk::parse_sfo(truncated, metadata));
    }
    for (size_t offset : {8, 12, 16, 24, 28, 32}) {
        auto damaged = valid; le(damaged, offset, 0xffffffff);
        assert(!vpk::parse_sfo(damaged, metadata));
    }
    fields["TITLE_ID"] = "../APP001"; assert(!vpk::parse_sfo(sfo(fields), metadata));
    fields["TITLE_ID"] = "TESTAPP01"; fields["CATEGORY"] = "gp"; assert(!vpk::parse_sfo(sfo(fields), metadata));
    fields["CATEGORY"] = "gd"; fields["CONTENT_ID"] = "EP9000-OTHERID01_00-0000000000000000";
    assert(!vpk::parse_sfo(sfo(fields), metadata));
    fields["CONTENT_ID"] = "EP9000-TESTAPP01_00-0000000000000000";
    assert(vpk::parse_sfo(sfo(fields), metadata));
    std::vector<uint8_t> head(0x200);
    for (size_t i = 0; i < head.size(); ++i) head[i] = i * 17 + 3;
    be(head, 8, 0x100); be(head, 0x10, 0x80); be(head, 0xd0, 0xc0); be(head, 0xd4, 0x180); be(head, 0xe8, 0x1e0);
    auto original = head;
    assert(vpk::make_head(head, metadata));
    // Vector independently generated with Python hashlib from the documented header digest format.
    unsigned char hash[SHA256_DIGEST_LENGTH]; SHA256(head.data(), head.size(), hash);
    char hex[65]{}; for (size_t i = 0; i < sizeof(hash); ++i) std::snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    assert(std::string(hex) == "9a2f594462ff0a3c93072bc07f71cca3df473dcf6a6edfb7c1b3b1c25dbfd9f0");
    for (size_t offset : {8, 0x10, 0xd0, 0xd4, 0xe8}) {
        auto damaged = original; be(damaged, offset, 0xffffffff);
        assert(!vpk::make_head(damaged, metadata));
    }
    std::vector<uint8_t> short_head(0xff); assert(!vpk::make_head(short_head, metadata));
    std::puts("VPK: metadata bounds, application IDs, package types, header digest vector and damaged headers: PASS");
}
