#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Primitives for the data vault. The VitaSDK OpenSSL is 1.0.2, which has neither ChaCha20-Poly1305
// nor scrypt, so both are implemented here from RFC 8439 / RFC 7914 (checked against the RFC
// vectors and the host OpenSSL in tests/vault_crypto_test.cpp).
//
// Why ChaCha20-Poly1305: the Vita's Cortex-A9 has no AES instructions, so AES would run as
// table lookups (slow, and with key-dependent memory access). ChaCha20 is add/rotate/xor only:
// constant time and roughly twice as fast as software AES-GCM on this core.
namespace vault_crypto {
constexpr size_t kKey = 32, kNonce = 12, kTag = 16;

// RFC 8439 AEAD. `out` may equal `in`. Tag is written separately.
void aead_seal(const uint8_t key[kKey], const uint8_t nonce[kNonce], const uint8_t* aad, size_t aad_len,
               const uint8_t* in, size_t len, uint8_t* out, uint8_t tag[kTag]);
// False (and `out` zeroed) when the tag does not verify. Constant-time tag comparison.
bool aead_open(const uint8_t key[kKey], const uint8_t nonce[kNonce], const uint8_t* aad, size_t aad_len,
               const uint8_t* in, size_t len, const uint8_t tag[kTag], uint8_t* out);

// Raw ChaCha20 block-counter keystream XOR (RFC 8439 section 2.4), exposed for tests.
void chacha20_xor(const uint8_t key[kKey], uint32_t counter, const uint8_t nonce[kNonce],
                  const uint8_t* in, size_t len, uint8_t* out);

void sha256(const void* data, size_t len, uint8_t out[32]);
void hmac_sha256(const void* key, size_t key_len, const void* data, size_t len, uint8_t out[32]);
// HKDF (RFC 5869) with SHA-256. `out_len` <= 255 * 32.
void hkdf(const uint8_t* ikm, size_t ikm_len, const std::string& salt, const std::string& info,
          uint8_t* out, size_t out_len);

// scrypt (RFC 7914). Needs 128 * r * N bytes (+ 128 * r * p). False on bad parameters or when
// memory cannot be allocated. `step`, when set, is called about every 1/64 of the work.
bool scrypt(const uint8_t* password, size_t password_len, const uint8_t* salt, size_t salt_len,
            uint32_t N, uint32_t r, uint32_t p, uint8_t* out, size_t out_len,
            void (*step)(void*) = nullptr, void* context = nullptr);

bool random_bytes(uint8_t* out, size_t len);
void wipe(void* data, size_t len);
bool equal_ct(const uint8_t* a, const uint8_t* b, size_t len);
std::string to_hex(const uint8_t* data, size_t len);
bool from_hex(const std::string& text, uint8_t* out, size_t len);
}
