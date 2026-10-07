#include "utf8_decode.h"
// vita2d's legacy decoder ignores four-byte sequences. Its font renderer and
// FreeType lookup already accept 32-bit codepoints, so replace only the decoder.
extern "C" int __wrap_utf8_to_ucs2(const char* text, unsigned int* code) {
    uint32_t value;
    int bytes = decode_utf8_scalar(text, value);
    *code = value;
    return bytes;
}
