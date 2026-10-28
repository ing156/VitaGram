#pragma once
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cstdint>
#include <unordered_map>
#include <vector>

struct vita2d_texture;

// One face, with independent rasterizations for every pixel size. The FT_Face
// and its memory buffer are owned by TextFace and must outlive this cache.
class TextFont {
public:
    inline static unsigned style = 0; // Synthetic bold/italic for the current UI text run.
    explicit TextFont(FT_Face face) : face_(face), default_size_(face->size) {}
    ~TextFont();
    TextFont(const TextFont&) = delete;
    TextFont& operator=(const TextFont&) = delete;
    int render(int x, int baseline, unsigned color, unsigned size, const char* text, bool draw);
    // Call between frames, after waiting for the GPU, if needs_reset() is true.
    bool needs_reset() const { return reset_pending_; }
    void reset();
private:
    struct Glyph {
        FT_UInt index = 0;
        FT_Pos advance = 0;
        int left = 0, top = 0, width = 0, height = 0;
        int page = -1, x = 0, y = 0; // -2: empty bitmap; no atlas entry needed.
    };
    struct Page { vita2d_texture* texture = nullptr; int x = 0, y = 0, row_height = 0; };
    FT_Face face_;
    FT_Size default_size_;
    std::unordered_map<unsigned, FT_Size> sizes_;
    std::unordered_map<uint64_t, Glyph> glyphs_;
    std::vector<Page> pages_;
    Glyph uncached_;
    bool reset_pending_ = false;
    Glyph* glyph(uint32_t code, unsigned size);
    void upload(Glyph& glyph);
    bool select_size(unsigned size);
};
