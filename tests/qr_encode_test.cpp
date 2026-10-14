#include "qr_encode.h"
#include <quirc.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

static bool roundtrip(const std::string& text, int* version_out = nullptr) {
    QrCode qr;
    if (!qr_encode(text, qr)) return false;
    const int scale = 4, quiet = 4, side = (qr.size + 2 * quiet) * scale;
    quirc* decoder = quirc_new();
    quirc_resize(decoder, side, side);
    int w, h;
    uint8_t* image = quirc_begin(decoder, &w, &h);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x)
            image[y * w + x] = qr.dark(x / scale - quiet, y / scale - quiet) ? 0 : 255;
    quirc_end(decoder);
    bool ok = false;
    if (quirc_count(decoder) == 1) {
        quirc_code code; quirc_data data;
        quirc_extract(decoder, 0, &code);
        ok = quirc_decode(&code, &data) == QUIRC_SUCCESS && data.payload_len == (int)text.size() &&
             !std::memcmp(data.payload, text.data(), text.size());
        if (version_out) *version_out = data.version;
    }
    quirc_destroy(decoder);
    return ok;
}

int main() {
    int version = 0;
    assert(roundtrip("https://github.com/ing156/VitaGram", &version));
    assert(roundtrip("tg://login?token=AbCdEfGhIjKlMnOpQrStUvWxYz0123456789_-AbCdEfGhIj", &version));
    // Every length from 1 byte up to the version-10 capacity must decode (checks every table entry).
    for (size_t n = 1; n <= 213; ++n) {
        std::string text;
        for (size_t i = 0; i < n; ++i) text += (char)(33 + (i * 7 + n) % 90);
        if (!roundtrip(text, &version)) { std::cerr << "failed at length " << n << " version " << version << "\n"; return 1; }
    }
    QrCode too_long;
    assert(!qr_encode(std::string(214, 'x'), too_long));
    std::cout << "QR encoder: byte mode, level M, versions 1-10 decode with quirc PASS\n";
}
