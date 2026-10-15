#pragma once
#include "qr_encode.h"

struct QrImage {
    int side = 0;
    std::vector<uint8_t> pixels; // Grayscale, white background, four-module quiet zone.
};

// Telegram-style rounded modules and finder rings, with a paper-plane badge.
// Pass a QR encoded with High error correction to allow for the central logo.
QrImage qr_render_login(const QrCode& qr, int module);
