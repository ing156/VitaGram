#include "qr_encode.h"
#include "qr_render.h"
#include <cstdlib>
#include <fstream>
#include <vector>
#include <quirc.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

static bool roundtrip(const std::string& text, int* version_out = nullptr, QrEcc ecc = QrEcc::Medium, bool styled = false, int module = 0) {
    QrCode qr;
    if (!qr_encode(text, qr, ecc)) return false;
    const int scale = module ? module : styled ? 400/(qr.size+8) : 4, quiet = 4, side = (qr.size + 2 * quiet) * scale;
    quirc* decoder = quirc_new();
    quirc_resize(decoder, side, side);
    int w, h;
    uint8_t* image = quirc_begin(decoder, &w, &h);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x)
            image[y * w + x] = qr.dark(x / scale - quiet, y / scale - quiet) ? 0 : 255;
    if (styled) {
        const auto rendered=qr_render_login(qr,scale);
        assert(rendered.side==side);
        for (int y=0; y<side; ++y) for (int x=0; x<side; ++x)
            if (x<4*scale || y<4*scale || x>=side-4*scale || y>=side-4*scale)
                assert(rendered.pixels[size_t(y)*side+x]==255);
        std::memcpy(image,rendered.pixels.data(),rendered.pixels.size());
        if (const char* path=std::getenv("VITAGRAM_QR_PREVIEW")) {
            std::ofstream file(path,std::ios::binary);
            file << "P5\n" << side << " " << side << "\n255\n";
            file.write(reinterpret_cast<const char*>(image),size_t(side)*side);
        }
    }
    const std::vector<uint8_t> original(image,image+side*side);
    bool ok = false;
    for (int attempt=0; attempt<(styled ? 2 : 1) && !ok; ++attempt) {
        if (attempt) std::memcpy(quirc_begin(decoder,nullptr,nullptr),original.data(),original.size());
        quirc_set_rounded_finders(decoder,attempt);
        quirc_end(decoder);
        for (int i=0; i<quirc_count(decoder); ++i) {
            quirc_code code; quirc_data data;
            quirc_extract(decoder,i,&code);
            if (quirc_decode(&code,&data)!=QUIRC_SUCCESS) continue;
            ok = data.payload_len==int(text.size()) && !std::memcmp(data.payload,text.data(),text.size());
            assert(data.ecc_level==(ecc==QrEcc::High ? QUIRC_ECC_LEVEL_H : QUIRC_ECC_LEVEL_M));
            if (version_out) *version_out = data.version;
        }
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
    for (size_t n=1; n<=119; ++n) {
        std::string text(n,'x');
        for (size_t i=0; i<n; ++i) text[i]=char(33+(i*7+n)%90);
        assert(roundtrip(text,&version,QrEcc::High));
    }
    // Decode the actual antialiased renderer with the central badge present.
    for (int length : {59,60,64,65,80,119}) for (int seed=0; seed<8; ++seed) {
        std::string text="tg://login?token=";
        while (int(text.size())<length) text += char('A'+(text.size()*7+seed)%26);
        if (!roundtrip(text,&version,QrEcc::High,true)) {
            std::cerr << "Styled QR failed: length=" << length << " seed=" << seed << "\n";
            return 1;
        }
    }
    assert(roundtrip("tg://login?token=AbCdEfGhIjKlMnOpQrStUvWxYz0123456789_-AbCdEfGhIj",
                     &version,QrEcc::High,true,4));
    QrCode too_long;
    assert(!qr_encode(std::string(120,'x'),too_long,QrEcc::High));
    assert(!qr_encode(std::string(214, 'x'), too_long));
    std::cout << "QR encoder: byte mode, levels M/H, versions 1-10 and styled login raster decode with quirc PASS\n";
}
