#include "secure_store.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <cstring>
#include <vector>
#ifdef __vita__
#include <psp2/kernel/openpsid.h>
#include <psp2/kernel/rng.h>
#endif

namespace {
const char kPrefix[] = "enc1:";
const size_t kPrefixLen = 5, kIv = 12, kTag = 16;

bool console_id(unsigned char out[16]) {
#ifdef __vita__
    SceKernelOpenPsId id{};
    if (sceKernelGetOpenPsId(&id) < 0) return false;
    std::memcpy(out, id.id, 16);
#else
    // Host builds (tests): fixed id, or none when VITAGRAM_TEST_NO_ID is defined at compile time.
#ifdef VITAGRAM_TEST_NO_ID
    return false;
#else
    std::memcpy(out, "host-test-id-123", 16);
#endif
#endif
    for (int i = 0; i < 16; ++i) if (out[i]) return true;
    return false;
}

// OpenSSL 1.0.2 has no entropy source on the Vita (RAND_bytes fails or is weakly seeded), so the
// kernel generator supplies the GCM IV; a repeated IV would break AES-GCM.
bool random_iv(unsigned char* out, size_t len) {
#ifdef __vita__
    return sceKernelGetRandomNumber(out, (SceSize)len) >= 0;
#else
    return RAND_bytes(out, (int)len) == 1;
#endif
}

bool derive(const char* label, unsigned char key[32]) {
    unsigned char id[16];
    if (!console_id(id)) return false;
    std::string input = std::string(label) + std::string((const char*)id, 16);
    SHA256((const unsigned char*)input.data(), input.size(), key);
    bool ok = true;
    return ok;
}

std::string to_hex(const unsigned char* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) { out += digits[data[i] >> 4]; out += digits[data[i] & 15]; }
    return out;
}

bool from_hex(const std::string& text, std::vector<unsigned char>& out) {
    if (text.size() % 2) return false;
    out.clear();
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < text.size(); i += 2) {
        int hi = nibble(text[i]), lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((unsigned char)(hi * 16 + lo));
    }
    return true;
}
}

namespace secure_store {
bool available() { unsigned char id[16]; return console_id(id); }
bool is_protected(const std::string& stored) { return stored.compare(0, kPrefixLen, kPrefix) == 0; }

std::string protect(const std::string& plain) {
    if (plain.empty() || is_protected(plain)) return plain;
    unsigned char key[32], iv[kIv], tag[kTag];
    if (!derive("VitaGram-settings-v1", key)) return plain; // no console id: documented plain-text mode
    if (!random_iv(iv, kIv)) return std::string(); // never store a secret unprotected or with a weak IV
    std::vector<unsigned char> cipher(plain.size());
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0, total = 0;
    bool ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, iv) &&
              EVP_EncryptUpdate(ctx, cipher.data(), &len, (const unsigned char*)plain.data(), (int)plain.size());
    total = len;
    ok = ok && EVP_EncryptFinal_ex(ctx, cipher.data() + total, &len) && (total += len) == (int)plain.size() &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTag, tag) == 1;
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    if (!ok) return std::string(); // fail closed: drop the value rather than write it in plain text
    return std::string(kPrefix) + to_hex(iv, kIv) + to_hex(tag, kTag) + to_hex(cipher.data(), cipher.size());
}

bool reveal(const std::string& stored, std::string& plain) {
    plain.clear();
    if (!is_protected(stored)) { plain = stored; return true; }
    std::vector<unsigned char> raw;
    if (!from_hex(stored.substr(kPrefixLen), raw) || raw.size() <= kIv + kTag) return false;
    unsigned char key[32];
    if (!derive("VitaGram-settings-v1", key)) return false;
    size_t size = raw.size() - kIv - kTag;
    std::vector<unsigned char> out(size);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0, total = 0;
    bool ok = ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, raw.data()) &&
              EVP_DecryptUpdate(ctx, out.data(), &len, raw.data() + kIv + kTag, (int)size);
    total = len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTag, raw.data() + kIv) == 1 &&
         EVP_DecryptFinal_ex(ctx, out.data() + total, &len) > 0 && (total += len) == (int)size;
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;
    plain.assign((const char*)out.data(), size);
    return true;
}

bool console_binding(unsigned char out[32]) { return derive("VitaGram-vault-bind-v1", out); }

std::string database_key() {
    unsigned char key[32];
    return derive("VitaGram-tdlib-v1", key) ? to_hex(key, 32) : "";
}
}
