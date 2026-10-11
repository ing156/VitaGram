#include "text_font.h"
#include "utf8_decode.h"
#include <vita2d.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr int atlas_size = 512;
constexpr int padding = 1;
constexpr size_t max_pages = 4; // At most 1 MiB of CDRAM per loaded text face.
constexpr size_t max_glyphs = 4096;
int pixels(FT_Pos value) { return static_cast<int>(std::lround(value / 64.0)); }
}

TextFont::~TextFont() { reset(); }
void TextFont::reset() {
    for (auto& page : pages_) vita2d_free_texture(page.texture);
    pages_.clear();
    glyphs_.clear();
    reset_pending_ = false;
}

TextFont::Glyph* TextFont::glyph(uint32_t code, unsigned size) {
    const uint64_t key = (uint64_t(size) << 32) | code;
    auto found = glyphs_.find(key);
    if (found != glyphs_.end()) return &found->second;
    const FT_UInt index = FT_Get_Char_Index(face_, code);
    if (FT_Load_Glyph(face_, index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) return nullptr;
    const auto slot = face_->glyph;
    Glyph value;
    value.index = index;
    value.advance = slot->advance.x;
    value.left = slot->bitmap_left;
    value.top = slot->bitmap_top;
    value.width = static_cast<int>(slot->bitmap.width);
    value.height = static_cast<int>(slot->bitmap.rows);
    if (glyphs_.size() >= max_glyphs) {
        reset_pending_ = true;
        uncached_ = value; // Keep measurements correct until the next between-frame reset.
        return &uncached_;
    }
    return &glyphs_.emplace(key, value).first->second;
}

void TextFont::upload(Glyph& glyph) {
    if (!glyph.width || !glyph.height || glyph.page >= 0) return;
    const int width = glyph.width + 2 * padding, height = glyph.height + 2 * padding;
    if (width > atlas_size || height > atlas_size) return;
    if (FT_Load_Glyph(face_, glyph.index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) return;
    const FT_Bitmap& bitmap = face_->glyph->bitmap;
    if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && bitmap.pixel_mode != FT_PIXEL_MODE_MONO) return;
    for (size_t i = 0; i <= pages_.size(); ++i) {
        if (i == pages_.size()) {
            if (pages_.size() >= max_pages) { reset_pending_ = true; return; }
            auto* texture = vita2d_create_empty_texture_format(atlas_size, atlas_size, SCE_GXM_TEXTURE_FORMAT_U8_R111);
            if (!texture) return;
            std::memset(vita2d_texture_get_datap(texture), 0,
                vita2d_texture_get_stride(texture) * atlas_size);
            // Glyphs are drawn at their native size on integer pixel positions.
            vita2d_texture_set_filters(texture, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
            pages_.push_back({texture, 0, 0, 0});
        }
        auto& page = pages_[i];
        if (page.x + width > atlas_size) { page.x = 0; page.y += page.row_height; page.row_height = 0; }
        if (page.y + height > atlas_size) continue;
        glyph.page = static_cast<int>(i);
        glyph.x = page.x + padding; glyph.y = page.y + padding;
        auto* target = static_cast<unsigned char*>(vita2d_texture_get_datap(page.texture));
        const auto stride = vita2d_texture_get_stride(page.texture);
        for (int y = 0; y < glyph.height; ++y) {
            const auto* source = bitmap.buffer + (bitmap.pitch >= 0 ? y * bitmap.pitch : (glyph.height - 1 - y) * -bitmap.pitch);
            auto* row = target + (glyph.y + y) * stride + glyph.x;
            if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) std::memcpy(row, source, glyph.width);
            else for (int x = 0; x < glyph.width; ++x) row[x] = (source[x / 8] & (0x80 >> (x % 8))) ? 255 : 0;
        }
        page.x += width; page.row_height = std::max(page.row_height, height);
        return;
    }
}

int TextFont::render(int x, int baseline, unsigned color, unsigned size, const char* text, bool draw) {
    if (!text || FT_Set_Pixel_Sizes(face_, 0, size)) return 0;
    FT_Pos pen = 0, maximum = 0;
    FT_UInt previous = 0;
    while (*text) {
        uint32_t code;
        text += decode_utf8_scalar(text, code);
        if (code == '\n') {
            maximum = std::max(maximum, pen); pen = 0; baseline += size; previous = 0; continue;
        }
        Glyph* current = glyph(code, size);
        if (!current) continue;
        if (previous && current->index && FT_HAS_KERNING(face_)) {
            FT_Vector delta{};
            if (!FT_Get_Kerning(face_, previous, current->index, FT_KERNING_DEFAULT, &delta)) pen += delta.x;
        }
        if (draw) {
            upload(*current);
            if (current->page >= 0) vita2d_draw_texture_tint_part_scale(pages_[current->page].texture,
                x + pixels(pen) + current->left, baseline - current->top,
                current->x, current->y, current->width, current->height, 1.f, 1.f, color);
        }
        pen += current->advance; previous = current->index;
    }
    return pixels(std::max(maximum, pen));
}
