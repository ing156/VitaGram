#include "font_fallback.h"
#include "utf8_decode.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <array>
#include <cassert>
#include <cstdio>
#include <string>
int main(int argc,char** argv) {
    assert(argc==2);
    const char* utf8 = u8"𝕭𝖆𝖉𝕯𝖗𝖊𝖆𝖒";
    for (char32_t expected : std::u32string(U"𝕭𝖆𝖉𝕯𝖗𝖊𝖆𝖒")) {
        unsigned int code = 0;
        assert(__wrap_utf8_to_ucs2(utf8, &code) == 4 && code == expected);
        utf8 += 4;
    }
    assert(!*utf8);
    unsigned int decoded = 0;
    assert(__wrap_utf8_to_ucs2(u8"ё", &decoded) == 2 && decoded == U'ё');
    assert(__wrap_utf8_to_ucs2(u8"宇", &decoded) == 3 && decoded == U'宇');
    assert(__wrap_utf8_to_ucs2("A", &decoded) == 1 && decoded == U'A');
    for (const char* bad : {"\xf0", "\xf0\x9d", "\xf0\x9d\x95", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xc0\x80"})
        assert(__wrap_utf8_to_ucs2(bad, &decoded) == 1 && decoded == 0xfffd);
    FT_Library library=nullptr; assert(!FT_Init_FreeType(&library));
    std::array<FT_Face,5> faces{};
    const char* paths[]={"NotoSans-Regular.ttf","NotoSansCoptic-Regular.ttf","NotoSansJP-Regular.otf","NotoSansMath-Regular.ttf","NotoSansSymbols2-Regular.ttf"};
    for(size_t i=0;i<faces.size();++i) {
        assert(!FT_New_Face(library,(std::string(argv[1])+"/"+paths[i]).c_str(),0,&faces[i]));
        assert(!FT_Set_Pixel_Sizes(faces[i],0,18));
    }
    assert(!FT_HAS_MULTIPLE_MASTERS(faces[2])); // Static Regular, not variable Thin.
    auto font=[&](uint32_t code) { return font_fallback_index(code,faces.size(),[&](size_t i,uint32_t c) {return FT_Get_Char_Index(faces[i],c)!=0;}); };
    for (const auto& name : {U"✧Hehestl | *interrogative hehe*",U"𝕭𝖆𝖉𝕯𝖗𝖊𝖆𝖒",U"ⲕⲗυⲙⲁⲧ",U"の一部 宇宙",U"АБВГД абвгд Ёё Latin 123"}) {
        for (char32_t c : std::u32string(name)) {
            size_t i=font(c);
            assert(FT_Get_Char_Index(faces[i],c));
            assert(!FT_Load_Char(faces[i],c,FT_LOAD_RENDER));
            assert(faces[i]->glyph->advance.x>0);
            if (c!=U' ') assert(faces[i]->glyph->bitmap.width>0 && faces[i]->glyph->bitmap.rows>0);
        }
    }
    assert(font(U'✧')==4);
    assert(font(U'𝕭')==3 && font(U'𝖒')==3);
    assert(font(U'ⲕ')==1 && font(U'υ')==0 && font(U'の')==2 && font(U'宇')==2 && font(U'ё')==0);
    assert(font(0x10ffff)==0); // Unsupported codepoint retains the base missing glyph.
    for(auto face:faces) FT_Done_Face(face);
    FT_Done_FreeType(library);
    std::puts("Fonts: all reported names including supplementary-plane Fraktur, fallback routing and real FreeType rasterization passed");
}
