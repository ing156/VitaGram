#pragma once
#include <cstdint>
#include <string>
#include <vector>

// QR encoder (ISO 18004): byte mode, versions 1-10.
// Medium: up to 213 bytes; High: up to 119 bytes, for login QR with a logo.
enum class QrEcc { Medium, High };
struct QrCode {
    int size = 0;                  // modules per side
    std::vector<uint8_t> modules;  // size*size, 1 = dark; row-major
    bool dark(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && modules[(size_t)y * size + x]; }
};
// False when the text does not fit.
bool qr_encode(const std::string& text, QrCode& out, QrEcc ecc = QrEcc::Medium);
