#include "text_font.h"
#include "utf8_decode.h"
#include <vita2d.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include FT_SIZES_H

static int live_pages = 0;
static unsigned rasterizations = 0;
static unsigned size_requests=0, live_sizes=0;
static bool size_allocation_error=false;
extern "C" FT_Error __real_FT_Set_Pixel_Sizes(FT_Face,FT_UInt,FT_UInt);
extern "C" FT_Error __wrap_FT_Set_Pixel_Sizes(FT_Face face,FT_UInt width,FT_UInt height) {
    ++size_requests;
    return __real_FT_Set_Pixel_Sizes(face,width,height);
}
extern "C" FT_Error __real_FT_New_Size(FT_Face,FT_Size*);
extern "C" FT_Error __wrap_FT_New_Size(FT_Face face,FT_Size* size) {
    if (size_allocation_error) return FT_Err_Out_Of_Memory;
    const auto result=__real_FT_New_Size(face,size);
    if (!result) ++live_sizes;
    return result;
}
extern "C" FT_Error __real_FT_Done_Size(FT_Size);
extern "C" FT_Error __wrap_FT_Done_Size(FT_Size size) {
    const auto result=__real_FT_Done_Size(size);
    if (!result) { assert(live_sizes); --live_sizes; }
    return result;
}
extern "C" FT_Error __real_FT_Render_Glyph(FT_GlyphSlot,FT_Render_Mode);
extern "C" FT_Error __wrap_FT_Render_Glyph(FT_GlyphSlot slot,FT_Render_Mode mode) {
    ++rasterizations;
    return __real_FT_Render_Glyph(slot,mode);
}
struct Drawing { int x, y, width, height; std::vector<unsigned char> pixels; };
static std::vector<Drawing> drawings;
vita2d_texture* vita2d_create_empty_texture_format(unsigned w, unsigned h, unsigned format) {
    assert(format == SCE_GXM_TEXTURE_FORMAT_U8_R111);
    ++live_pages;
    // Include nonzero row padding to exercise stride-aware writes and clearing.
    return new vita2d_texture{w, h, w + 16, std::vector<unsigned char>((w + 16) * h, 123)};
}
void* vita2d_texture_get_datap(vita2d_texture* texture) { return texture->pixels.data(); }
unsigned vita2d_texture_get_stride(vita2d_texture* texture) { return texture->stride; }
void vita2d_texture_set_filters(vita2d_texture*, unsigned min, unsigned mag) {
    assert(min == SCE_GXM_TEXTURE_FILTER_POINT && mag == SCE_GXM_TEXTURE_FILTER_POINT);
}
void vita2d_free_texture(vita2d_texture* texture) { --live_pages; delete texture; }
void vita2d_draw_texture_tint_part_scale(const vita2d_texture* texture, float x, float y, float tx, float ty,
                                       float w, float h, float sx, float sy, unsigned) {
    assert(sx == 1 && sy == 1 && x == std::round(x) && y == std::round(y));
    assert(tx >= 1 && ty >= 1 && tx + w < texture->width && ty + h < texture->height);
    Drawing drawing{int(x), int(y), int(w), int(h), {}};
    for (int row = -1; row <= int(h); ++row) {
        for (int col = -1; col <= int(w); ++col) {
            const auto pixel = texture->pixels[(int(ty) + row) * texture->stride + int(tx) + col];
            if (row == -1 || row == int(h) || col == -1 || col == int(w)) assert(pixel == 0);
            else drawing.pixels.push_back(pixel);
        }
    }
    drawings.push_back(std::move(drawing));
}
static void equal(const std::vector<Drawing>& a, const std::vector<Drawing>& b) {
    assert(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        assert(a[i].x == b[i].x && a[i].y == b[i].y && a[i].width == b[i].width && a[i].height == b[i].height);
        assert(a[i].pixels == b[i].pixels);
    }
}
static void reference(FT_Face face, unsigned size, const char* text) {
    assert(!FT_Set_Pixel_Sizes(face, 0, size));
    size_t n = 0;
    FT_Pos pen = 0;
    FT_UInt previous = 0;
    while (*text) {
        uint32_t code; text += decode_utf8_scalar(text, code);
        const auto index = FT_Get_Char_Index(face, code);
        if (previous && index && FT_HAS_KERNING(face)) {
            FT_Vector delta{}; assert(!FT_Get_Kerning(face, previous, index, FT_KERNING_DEFAULT, &delta)); pen += delta.x;
        }
        assert(!FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL));
        const auto glyph = face->glyph;
        const auto& bitmap = glyph->bitmap;
        if (bitmap.width && bitmap.rows) {
            assert(n < drawings.size()); const auto& drawing = drawings[n++];
            assert(drawing.x == 34 + int(std::lround(pen / 64.0)) + glyph->bitmap_left);
            assert(drawing.y == 102 - glyph->bitmap_top);
            assert(drawing.width == int(bitmap.width) && drawing.height == int(bitmap.rows));
            for (unsigned y = 0; y < bitmap.rows; ++y) for (unsigned x = 0; x < bitmap.width; ++x)
                assert(drawing.pixels[y * bitmap.width + x] == bitmap.buffer[y * bitmap.pitch + x]);
        }
        pen += glyph->advance.x; previous = index;
    }
    assert(n == drawings.size());
}
int main(int argc, char** argv) {
    assert(argc == 2);
    FT_Library library{}; assert(!FT_Init_FreeType(&library));
    FT_Face face{}, reference_face{};
    const auto path = std::string(argv[1]) + "/NotoSans-Regular.ttf";
    assert(!FT_New_Face(library, path.c_str(), 0, &face));
    assert(!FT_New_Face(library, path.c_str(), 0, &reference_face));
    const auto original_size=face->size;
    {
        TextFont font(face);
        const auto before=size_requests;
        for (unsigned i=0;i<100;++i) {
            assert(font.render(0,0,0,26,"AV",false)>0);
            assert(font.render(0,0,0,12,"AV",false)>0);
        }
        assert(size_requests-before==2 && live_sizes==2);
        std::puts("Text size switching: 200 alternating measurements need only 2 size configurations");
        size_allocation_error=true;
        assert(font.render(0,0,0,27,"AV",false)>0 && live_sizes==2);
        size_allocation_error=false;
        assert(font.render(0,0,0,26,"AV",false)>0);
        for (unsigned size=1;size<=90;++size) font.render(0,0,0,size,"A",false);
        assert(live_sizes==64);
        font.reset(); assert(live_sizes==0 && face->size==original_size);
    }
    rasterizations=0;
    {
        TextFont font(face);
        const char* text = u8"Книга жалоб ПРАВИЛА [закрыта] PSVITA AV To ёй";
        assert(font.render(0, 0, 0, 15, text, false) > 0);
        assert(rasterizations == 0); // Width-only queries must not rasterize.
        assert(live_pages == 0); // Measurement must not populate the GPU atlas.
        std::vector<Drawing> baseline;
        int baseline_width = 0;
        for (unsigned size : {25u, 15u, 22u, 34u, 25u}) {
            drawings.clear();
            const int width = font.render(34, 102, 0xffffffff, size, text, true);
            assert(width == font.render(0, 0, 0, size, text, false));
            reference(reference_face, size, text);
            if (size == 25 && baseline.empty()) { baseline = drawings; baseline_width = width; }
            else if (size == 25) { equal(baseline, drawings); assert(width == baseline_width); }
        }
        for (unsigned style : {1u, 2u, 3u}) {
            TextFont::style = style;
            drawings.clear();
            assert(font.render(34, 102, 0xffffffff, 25, text, true) == baseline_width);
            assert(!drawings.empty());
        }
        TextFont::style = 0;
        drawings.clear();
        font.render(34, 102, 0xffffffff, 25, text, true);
        equal(baseline, drawings);
        const auto drawn_rasters=rasterizations;
        font.render(34,102,0xffffffff,25,text,true);
        assert(rasterizations==drawn_rasters);
        font.render(0,0,0,17,text,false);
        assert(rasterizations==drawn_rasters);
        font.render(34,102,0xffffffff,17,text,true);
        assert(rasterizations>drawn_rasters);
        font.reset(); assert(live_pages == 0);
        font.render(0, 0, 0, 34, text, false);
        drawings.clear();
        assert(font.render(34, 102, 0xffffffff, 25, text, true) == baseline_width);
        equal(baseline, drawings); // First-use size and measurement order cannot change pixels.
        assert(font.render(0, 0, 0, 25, "AV\nAV", false) == font.render(0, 0, 0, 25, "AV", false));
        assert(font.render(0, 0, 0, 25, " ", true) > 0);
        for (uint32_t code = 0x400; code < 0x400 + 4200; ++code) {
            // Most codes map to .notdef: every scalar still has its own bounded entry.
            std::string scalar;
            if (code < 0x800) { scalar += char(0xc0 | (code >> 6)); scalar += char(0x80 | (code & 63)); }
            else { scalar += char(0xe0 | (code >> 12)); scalar += char(0x80 | ((code >> 6) & 63)); scalar += char(0x80 | (code & 63)); }
            font.render(0, 0, 0, 48, scalar.c_str(), true);
        }
        assert(live_pages <= 4 && font.needs_reset());
        font.reset(); assert(live_pages == 0 && !font.needs_reset());
    }
    assert(live_pages == 0);
    assert(live_sizes == 0 && face->size==original_size);
    FT_Done_Face(reference_face); FT_Done_Face(face); FT_Done_FreeType(library);
    std::puts("Text raster cache: width-only queries skip rasterization; cached drawing, exact pixels, styles, kerning and bounded reset passed");
}
