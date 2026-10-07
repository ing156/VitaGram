#pragma once
#include <cstddef>
#include <cstdint>
// Shared by measurement and drawing. Coverage is checked per Unicode scalar,
// rather than routing whole names by an assumed language.
template<class HasGlyph>
size_t font_fallback_index(uint32_t code, size_t count, HasGlyph has_glyph) {
    for (size_t i=0; i<count; ++i) if (has_glyph(i,code)) return i;
    return 0;
}
