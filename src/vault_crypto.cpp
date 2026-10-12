#include "vault_crypto.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef __vita__
#include <psp2/kernel/rng.h>
#endif

// All supported targets (ARMv7 Vita, x86-64 host tests) are little-endian; the scrypt block code
// below relies on it.
namespace {
inline uint32_t rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
inline uint32_t load32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
inline void store32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
inline void store64(uint8_t* p, uint64_t v) { store32(p, (uint32_t)v); store32(p + 4, (uint32_t)(v >> 32)); }

// ---- ChaCha20 (RFC 8439) -------------------------------------------------------------------
#define QR(a, b, c, d) \
    a += b; d ^= a; d = rotl(d, 16); c += d; b ^= c; b = rotl(b, 12); \
    a += b; d ^= a; d = rotl(d, 8);  c += d; b ^= c; b = rotl(b, 7)

void chacha_block(const uint32_t in[16], uint8_t out[64]) {
    uint32_t x[16];
    std::memcpy(x, in, sizeof(x));
    for (int i = 0; i < 10; ++i) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i) store32(out + 4 * i, x[i] + in[i]);
}

void chacha_init(uint32_t s[16], const uint8_t* key, uint32_t counter, const uint8_t* nonce) {
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; ++i) s[4 + i] = load32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; ++i) s[13 + i] = load32(nonce + 4 * i);
}

// ---- Poly1305 (26-bit limbs, 32-bit multiplies; public-domain "donna" layout) ----------------
struct Poly1305 {
    uint32_t r[5], h[5], pad[4];
    uint8_t buffer[16];
    size_t leftover = 0;

    void init(const uint8_t key[32]) {
        r[0] = load32(key) & 0x3ffffff;
        r[1] = (load32(key + 3) >> 2) & 0x3ffff03;
        r[2] = (load32(key + 6) >> 4) & 0x3ffc0ff;
        r[3] = (load32(key + 9) >> 6) & 0x3f03fff;
        r[4] = (load32(key + 12) >> 8) & 0x00fffff;
        std::memset(h, 0, sizeof(h));
        for (int i = 0; i < 4; ++i) pad[i] = load32(key + 16 + 4 * i);
        leftover = 0;
    }
    void blocks(const uint8_t* m, size_t bytes, uint32_t hibit) {
        const uint32_t r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4];
        const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
        uint32_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4];
        while (bytes >= 16) {
            h0 += load32(m) & 0x3ffffff;
            h1 += (load32(m + 3) >> 2) & 0x3ffffff;
            h2 += (load32(m + 6) >> 4) & 0x3ffffff;
            h3 += (load32(m + 9) >> 6) & 0x3ffffff;
            h4 += (load32(m + 12) >> 8) | hibit;
            uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
            uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
            uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
            uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
            uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
            uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
            d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
            d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
            d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
            d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
            h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
            m += 16; bytes -= 16;
        }
        h[0] = h0; h[1] = h1; h[2] = h2; h[3] = h3; h[4] = h4;
    }
    void update(const uint8_t* m, size_t bytes) {
        if (leftover) {
            size_t want = 16 - leftover;
            if (want > bytes) want = bytes;
            std::memcpy(buffer + leftover, m, want);
            bytes -= want; m += want; leftover += want;
            if (leftover < 16) return;
            blocks(buffer, 16, 1u << 24);
            leftover = 0;
        }
        if (bytes >= 16) {
            size_t whole = bytes & ~(size_t)15;
            blocks(m, whole, 1u << 24);
            m += whole; bytes -= whole;
        }
        if (bytes) { std::memcpy(buffer, m, bytes); leftover = bytes; }
    }
    void finish(uint8_t mac[16]) {
        if (leftover) {
            buffer[leftover++] = 1;
            while (leftover < 16) buffer[leftover++] = 0;
            blocks(buffer, 16, 0);
        }
        uint32_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4], c;
        c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
        c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
        c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
        c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
        c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
        uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
        uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
        uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
        uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
        uint32_t g4 = h4 + c - (1u << 26);
        uint32_t mask = (g4 >> 31) - 1; // all ones when h >= p
        g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
        mask = ~mask;
        h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
        h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
        h0 = (h0 | (h1 << 26)) & 0xffffffff;
        h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff;
        h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffff;
        h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffff;
        uint64_t f = (uint64_t)h0 + pad[0]; store32(mac, (uint32_t)f);
        f = (uint64_t)h1 + pad[1] + (f >> 32); store32(mac + 4, (uint32_t)f);
        f = (uint64_t)h2 + pad[2] + (f >> 32); store32(mac + 8, (uint32_t)f);
        f = (uint64_t)h3 + pad[3] + (f >> 32); store32(mac + 12, (uint32_t)f);
        vault_crypto::wipe(this, sizeof(*this));
    }
};

void poly_pad(Poly1305& p, size_t len) {
    static const uint8_t zeros[16] = {};
    if (len % 16) p.update(zeros, 16 - len % 16);
}

void aead_tag(const uint8_t otk[32], const uint8_t* aad, size_t aad_len, const uint8_t* ct, size_t len, uint8_t tag[16]) {
    Poly1305 p;
    p.init(otk);
    p.update(aad, aad_len); poly_pad(p, aad_len);
    p.update(ct, len); poly_pad(p, len);
    uint8_t lengths[16];
    store64(lengths, aad_len); store64(lengths + 8, len);
    p.update(lengths, 16);
    p.finish(tag);
}

// ---- scrypt ----------------------------------------------------------------------------------
#define R(a, b) rotl((a), (b))
void salsa20_8(uint32_t B[16]) {
    uint32_t x[16];
    std::memcpy(x, B, sizeof(x));
    for (int i = 0; i < 8; i += 2) {
        x[4] ^= R(x[0] + x[12], 7);  x[8] ^= R(x[4] + x[0], 9);
        x[12] ^= R(x[8] + x[4], 13); x[0] ^= R(x[12] + x[8], 18);
        x[9] ^= R(x[5] + x[1], 7);   x[13] ^= R(x[9] + x[5], 9);
        x[1] ^= R(x[13] + x[9], 13); x[5] ^= R(x[1] + x[13], 18);
        x[14] ^= R(x[10] + x[6], 7); x[2] ^= R(x[14] + x[10], 9);
        x[6] ^= R(x[2] + x[14], 13); x[10] ^= R(x[6] + x[2], 18);
        x[3] ^= R(x[15] + x[11], 7); x[7] ^= R(x[3] + x[15], 9);
        x[11] ^= R(x[7] + x[3], 13); x[15] ^= R(x[11] + x[7], 18);
        x[1] ^= R(x[0] + x[3], 7);   x[2] ^= R(x[1] + x[0], 9);
        x[3] ^= R(x[2] + x[1], 13);  x[0] ^= R(x[3] + x[2], 18);
        x[6] ^= R(x[5] + x[4], 7);   x[7] ^= R(x[6] + x[5], 9);
        x[4] ^= R(x[7] + x[6], 13);  x[5] ^= R(x[4] + x[7], 18);
        x[11] ^= R(x[10] + x[9], 7); x[8] ^= R(x[11] + x[10], 9);
        x[9] ^= R(x[8] + x[11], 13); x[10] ^= R(x[9] + x[8], 18);
        x[12] ^= R(x[15] + x[14], 7); x[13] ^= R(x[12] + x[15], 9);
        x[14] ^= R(x[13] + x[12], 13); x[15] ^= R(x[14] + x[13], 18);
    }
    for (int i = 0; i < 16; ++i) B[i] += x[i];
}
#undef R

// B and Y hold 2r 64-byte blocks (32 * r words each).
void block_mix(uint32_t* B, uint32_t* Y, uint32_t r) {
    uint32_t X[16];
    std::memcpy(X, B + (2 * r - 1) * 16, sizeof(X));
    for (uint32_t i = 0; i < 2 * r; ++i) {
        for (int k = 0; k < 16; ++k) X[k] ^= B[i * 16 + k];
        salsa20_8(X);
        std::memcpy(Y + i * 16, X, sizeof(X));
    }
    for (uint32_t i = 0; i < r; ++i) {
        std::memcpy(B + i * 16, Y + (2 * i) * 16, 64);
        std::memcpy(B + (i + r) * 16, Y + (2 * i + 1) * 16, 64);
    }
}

void ro_mix(uint32_t* B, uint32_t N, uint32_t r, uint32_t* V, uint32_t* Y, void (*step)(void*), void* context) {
    const size_t words = 32 * (size_t)r;
    const uint32_t tick = N >= 64 ? N / 64 : 1;
    for (uint32_t i = 0; i < N; ++i) {
        std::memcpy(V + i * words, B, words * 4);
        block_mix(B, Y, r);
        if (step && i % tick == 0) step(context);
    }
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t j = B[(2 * r - 1) * 16] & (N - 1);
        const uint32_t* v = V + (size_t)j * words;
        for (size_t k = 0; k < words; ++k) B[k] ^= v[k];
        block_mix(B, Y, r);
        if (step && i % tick == 0) step(context);
    }
}
}  // namespace

namespace vault_crypto {
void chacha20_xor(const uint8_t key[kKey], uint32_t counter, const uint8_t nonce[kNonce],
                  const uint8_t* in, size_t len, uint8_t* out) {
    uint32_t state[16];
    uint8_t block[64];
    chacha_init(state, key, counter, nonce);
    // Whole blocks: mix and XOR word by word, no intermediate byte buffer (the hot path while
    // a sealed video is being played).
    while (len >= 64) {
        uint32_t x[16];
        std::memcpy(x, state, sizeof(x));
        for (int i = 0; i < 10; ++i) {
            QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]);
            QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
            QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
            QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
        }
        for (int i = 0; i < 16; ++i) {
            uint32_t word;
            std::memcpy(&word, in + 4 * i, 4);
            word ^= x[i] + state[i]; // little-endian targets only (see the note at the top)
            std::memcpy(out + 4 * i, &word, 4);
        }
        ++state[12];
        in += 64; out += 64; len -= 64;
    }
    if (len) {
        chacha_block(state, block);
        for (size_t i = 0; i < len; ++i) out[i] = in[i] ^ block[i];
    }
    wipe(state, sizeof(state)); wipe(block, sizeof(block));
}

void aead_seal(const uint8_t key[kKey], const uint8_t nonce[kNonce], const uint8_t* aad, size_t aad_len,
               const uint8_t* in, size_t len, uint8_t* out, uint8_t tag[kTag]) {
    uint8_t otk[64];
    uint32_t state[16];
    chacha_init(state, key, 0, nonce);
    chacha_block(state, otk); // block 0 -> one-time Poly1305 key
    chacha20_xor(key, 1, nonce, in, len, out);
    aead_tag(otk, aad, aad_len, out, len, tag);
    wipe(otk, sizeof(otk)); wipe(state, sizeof(state));
}

bool aead_open(const uint8_t key[kKey], const uint8_t nonce[kNonce], const uint8_t* aad, size_t aad_len,
               const uint8_t* in, size_t len, const uint8_t tag[kTag], uint8_t* out) {
    uint8_t otk[64], expected[kTag];
    uint32_t state[16];
    chacha_init(state, key, 0, nonce);
    chacha_block(state, otk);
    aead_tag(otk, aad, aad_len, in, len, expected);
    bool ok = equal_ct(expected, tag, kTag);
    wipe(otk, sizeof(otk)); wipe(state, sizeof(state));
    if (!ok) { if (len) std::memset(out, 0, len); return false; }
    chacha20_xor(key, 1, nonce, in, len, out);
    return true;
}

void sha256(const void* data, size_t len, uint8_t out[32]) { SHA256((const unsigned char*)data, len, out); }

void hmac_sha256(const void* key, size_t key_len, const void* data, size_t len, uint8_t out[32]) {
    unsigned int n = 32;
    HMAC(EVP_sha256(), key, (int)key_len, (const unsigned char*)data, len, out, &n);
}

void hkdf(const uint8_t* ikm, size_t ikm_len, const std::string& salt, const std::string& info,
          uint8_t* out, size_t out_len) {
    uint8_t prk[32], t[32];
    // Extract. An empty salt means HashLen zero bytes.
    uint8_t zeros[32] = {};
    if (salt.empty()) hmac_sha256(zeros, 32, ikm, ikm_len, prk);
    else hmac_sha256(salt.data(), salt.size(), ikm, ikm_len, prk);
    std::string block;
    size_t done = 0, previous = 0;
    for (uint8_t counter = 1; done < out_len; ++counter) {
        block.assign((const char*)t, previous);
        block += info;
        block.push_back((char)counter);
        hmac_sha256(prk, 32, block.data(), block.size(), t);
        previous = 32;
        size_t n = out_len - done < 32 ? out_len - done : 32;
        std::memcpy(out + done, t, n);
        done += n;
    }
    wipe(prk, sizeof(prk)); wipe(t, sizeof(t));
    if (!block.empty()) wipe(&block[0], block.size());
}

bool scrypt(const uint8_t* password, size_t password_len, const uint8_t* salt, size_t salt_len,
            uint32_t N, uint32_t r, uint32_t p, uint8_t* out, size_t out_len,
            void (*step)(void*), void* context) {
    if (N < 2 || (N & (N - 1)) || !r || !p || r > 32 || p > 16 || N > (1u << 22)) return false;
    // The V array is 128*r*N bytes. Computed in 64 bits (size_t is 32-bit on the Vita) and capped at
    // kScryptMaxMemory, so no parameters can overflow the allocation size or ask for more than the
    // console can give.
    if ((uint64_t)128 * r * N > kScryptMaxMemory || (uint64_t)128 * r * p > kScryptMaxMemory) return false;
    const size_t block_bytes = 128 * (size_t)r;
    std::vector<uint8_t> B(block_bytes * p);
    if (!PKCS5_PBKDF2_HMAC((const char*)password, (int)password_len, salt, (int)salt_len, 1, EVP_sha256(),
                           (int)B.size(), B.data())) return false;
    auto* V = static_cast<uint32_t*>(std::malloc(block_bytes * N));
    auto* Y = static_cast<uint32_t*>(std::malloc(block_bytes));
    if (!V || !Y) { std::free(V); std::free(Y); wipe(B.data(), B.size()); return false; }
    std::vector<uint32_t> words(32 * (size_t)r);
    for (uint32_t i = 0; i < p; ++i) {
        std::memcpy(words.data(), B.data() + i * block_bytes, block_bytes);
        ro_mix(words.data(), N, r, V, Y, step, context);
        std::memcpy(B.data() + i * block_bytes, words.data(), block_bytes);
    }
    bool ok = PKCS5_PBKDF2_HMAC((const char*)password, (int)password_len, B.data(), (int)B.size(), 1, EVP_sha256(),
                                (int)out_len, out) == 1;
    wipe(V, block_bytes * N); wipe(Y, block_bytes); wipe(words.data(), words.size() * 4); wipe(B.data(), B.size());
    std::free(V); std::free(Y);
    return ok;
}

bool random_bytes(uint8_t* out, size_t len) {
#ifdef __vita__
    // The kernel generator directly; the bundled OpenSSL is seeded from it as well.
    while (len) {
        size_t n = len < 256 ? len : 256;
        if (sceKernelGetRandomNumber(out, (SceSize)n) < 0) return false;
        out += n; len -= n;
    }
    return true;
#else
    return RAND_bytes(out, (int)len) == 1;
#endif
}

void wipe(void* data, size_t len) { OPENSSL_cleanse(data, len); }

bool equal_ct(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

std::string to_hex(const uint8_t* data, size_t len) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) { out += digits[data[i] >> 4]; out += digits[data[i] & 15]; }
    return out;
}

bool from_hex(const std::string& text, uint8_t* out, size_t len) {
    if (text.size() != len * 2) return false;
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < len; ++i) {
        int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return true;
}
}
