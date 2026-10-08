#pragma once
#include <string>

// Obfuscation-grade protection for secrets stored on the memory card: AES-256-GCM with a key
// derived from this console's OpenPSID. A copied settings.json or TDLib database is useless on
// another console; code running on the same console can still derive the key.
namespace secure_store {
// False when no console id is available; then values are stored as plain text.
bool available();
// "enc1:<hex>" for non-empty input, "" for empty, the input unchanged when unavailable.
std::string protect(const std::string& plain);
// Accepts "enc1:" values and legacy plain text. Returns false when an "enc1:" value cannot be
// decrypted (other console, corrupt); `plain` is then empty.
bool reveal(const std::string& stored, std::string& plain);
bool is_protected(const std::string& stored);
// Stable 64-hex-character key for TDLib's database_encryption_key; "" when unavailable.
std::string database_key();
}
