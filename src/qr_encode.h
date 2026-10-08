#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Minimal QR Code encoder (ISO 18004): byte mode, error correction level M, versions 1-10
// (up to 213 bytes). Enough for Telegram's tg://login?token=... link (about 60 bytes).
struct QrCode {
    int size = 0;                  // modules per side
    std::vector<uint8_t> modules;  // size*size, 1 = dark; row-major
    bool dark(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && modules[(size_t)y * size + x]; }
};
// False when the text does not fit.
bool qr_encode(const std::string& text, QrCode& out);
