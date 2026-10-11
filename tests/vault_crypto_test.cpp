#include "vault_crypto.h"
#include <openssl/evp.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

using namespace vault_crypto;

static std::string hex(const std::vector<uint8_t>& v) { return to_hex(v.data(), v.size()); }
static std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> v(s.size() / 2);
    assert(from_hex(s, v.data(), v.size()));
    return v;
}

// Reference AEAD from the host OpenSSL (3.x has ChaCha20-Poly1305).
static void reference_seal(const uint8_t* key, const uint8_t* nonce, const std::vector<uint8_t>& aad,
                           const std::vector<uint8_t>& plain, std::vector<uint8_t>& cipher, uint8_t tag[16]) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    cipher.resize(plain.size());
    assert(EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, key, nonce));
    if (!aad.empty()) assert(EVP_EncryptUpdate(ctx, nullptr, &len, aad.data(), (int)aad.size()));
    if (!plain.empty()) assert(EVP_EncryptUpdate(ctx, cipher.data(), &len, plain.data(), (int)plain.size()));
    assert(EVP_EncryptFinal_ex(ctx, nullptr, &len));
    assert(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag));
    EVP_CIPHER_CTX_free(ctx);
}

int main() {
    // RFC 8439 section 2.8.2 AEAD test vector.
    {
        auto key = unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
        auto nonce = unhex("070000004041424344454647");
        auto aad = unhex("50515253c0c1c2c3c4c5c6c7");
        const std::string text = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
        std::vector<uint8_t> plain(text.begin(), text.end()), cipher(plain.size());
        uint8_t tag[16];
        aead_seal(key.data(), nonce.data(), aad.data(), aad.size(), plain.data(), plain.size(), cipher.data(), tag);
        assert(hex(std::vector<uint8_t>(tag, tag + 16)) == "1ae10b594f09e26a7e902ecbd0600691");
        assert(hex(cipher).compare(0, 32, "d31a8d34648e60db7b86afbc53ef7ec2") == 0);
        std::vector<uint8_t> back(plain.size());
        assert(aead_open(key.data(), nonce.data(), aad.data(), aad.size(), cipher.data(), cipher.size(), tag, back.data()));
        assert(back == plain);
    }
    // Randomised cross-check against OpenSSL, covering every length around block boundaries.
    {
        std::mt19937_64 rng(42);
        auto fill = [&](std::vector<uint8_t>& v) { for (auto& b : v) b = (uint8_t)rng(); };
        for (size_t len : {0, 1, 15, 16, 17, 31, 63, 64, 65, 127, 128, 129, 255, 1000, 4096, 65536, 65537}) {
            for (size_t aad_len : {0, 1, 12, 16, 33}) {
                std::vector<uint8_t> key(32), nonce(12), aad(aad_len), plain(len), ref, mine(len);
                fill(key); fill(nonce); fill(aad); fill(plain);
                uint8_t ref_tag[16], tag[16];
                reference_seal(key.data(), nonce.data(), aad, plain, ref, ref_tag);
                aead_seal(key.data(), nonce.data(), aad.data(), aad.size(), plain.data(), plain.size(), mine.data(), tag);
                assert(ref == mine && !std::memcmp(ref_tag, tag, 16));
                std::vector<uint8_t> back(len);
                assert(aead_open(key.data(), nonce.data(), aad.data(), aad.size(), mine.data(), len, tag, back.data()));
                assert(back == plain);
                // Any flipped bit in the ciphertext, tag or AAD must be rejected.
                if (len) { auto bad = mine; bad[len / 2] ^= 1; assert(!aead_open(key.data(), nonce.data(), aad.data(), aad.size(), bad.data(), len, tag, back.data())); }
                uint8_t bad_tag[16]; std::memcpy(bad_tag, tag, 16); bad_tag[15] ^= 0x80;
                assert(!aead_open(key.data(), nonce.data(), aad.data(), aad.size(), mine.data(), len, bad_tag, back.data()));
                if (aad_len) { auto bad_aad = aad; bad_aad[0] ^= 1; assert(!aead_open(key.data(), nonce.data(), bad_aad.data(), bad_aad.size(), mine.data(), len, tag, back.data())); }
            }
        }
    }
    // scrypt: RFC 7914 vectors, then agreement with OpenSSL's implementation.
    {
        uint8_t out[64];
        assert(scrypt((const uint8_t*)"", 0, (const uint8_t*)"", 0, 16, 1, 1, out, 64));
        assert(to_hex(out, 64) == "77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906");
        assert(scrypt((const uint8_t*)"password", 8, (const uint8_t*)"NaCl", 4, 1024, 8, 16, out, 64));
        assert(to_hex(out, 64) == "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b3731622eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640");
        uint8_t mine[32], ref[32];
        assert(scrypt((const uint8_t*)"\x00\x01\x02\x03\x04\x05\x06\x07", 8, (const uint8_t*)"0123456789abcdef", 16, 1 << 15, 8, 1, mine, 32));
        assert(EVP_PBE_scrypt("\x00\x01\x02\x03\x04\x05\x06\x07", 8, (const unsigned char*)"0123456789abcdef", 16, 1 << 15, 8, 1, 64u << 20, ref, 32));
        assert(!std::memcmp(mine, ref, 32));
        assert(!scrypt((const uint8_t*)"x", 1, (const uint8_t*)"y", 1, 1000, 8, 1, mine, 32)); // N not a power of two
        assert(!scrypt((const uint8_t*)"x", 1, (const uint8_t*)"y", 1, 1u << 17, 8, 1, mine, 32)); // 128 MiB: over the cap
        assert(!scrypt((const uint8_t*)"x", 1, (const uint8_t*)"y", 1, 1u << 22, 32, 1, mine, 32)); // 16 GiB: would overflow 32-bit size_t
    }
    // HKDF RFC 5869 test case 1.
    {
        auto ikm = unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
        auto salt = unhex("000102030405060708090a0b0c");
        auto info = unhex("f0f1f2f3f4f5f6f7f8f9");
        uint8_t okm[42];
        hkdf(ikm.data(), ikm.size(), std::string(salt.begin(), salt.end()), std::string(info.begin(), info.end()), okm, 42);
        assert(to_hex(okm, 42) == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    }
    // Throughput (informational; the Vita's Cortex-A9 is roughly 10-20x slower than this host).
    {
        std::vector<uint8_t> key(32, 7), nonce(12, 1), buf(1 << 20, 3);
        uint8_t tag[16];
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 64; ++i) aead_seal(key.data(), nonce.data(), nullptr, 0, buf.data(), buf.size(), buf.data(), tag);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "ChaCha20-Poly1305 host throughput: " << 64.0 / secs << " MiB/s\n";
        uint8_t out[32];
        start = std::chrono::steady_clock::now();
        assert(scrypt((const uint8_t*)"pin", 3, (const uint8_t*)"salt", 4, 1 << 15, 8, 1, out, 32));
        std::cout << "scrypt N=2^15 r=8 host time: "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s\n";
    }
    std::cout << "Vault crypto: RFC 8439 / 7914 / 5869 vectors, OpenSSL cross-check, tamper rejection PASS\n";
}
