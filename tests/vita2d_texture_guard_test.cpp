#include <vita2d.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <initializer_list>

extern "C" vita2d_texture* __wrap_vita2d_create_empty_texture(unsigned int, unsigned int);
extern "C" vita2d_texture* __wrap_vita2d_create_empty_texture_format(unsigned int, unsigned int, SceGxmTextureFormat);

namespace {
vita2d_texture texture;
bool allocation_failed = false;
unsigned calls = 0;
SceGxmTextureFormat last_format;

vita2d_texture* allocate(unsigned width, unsigned height, SceGxmTextureFormat format) {
    assert(width == 32 && height == 16);
    ++calls;
    last_format = format;
    if (allocation_failed) return nullptr;
    // Reused heap memory can contain another texture's target pointer and depth UID.
    std::memset(&texture, 0xA5, sizeof(texture));
    texture.data_UID = 101;
    texture.palette_UID = format == SCE_GXM_TEXTURE_FORMAT_P8_ABGR ? 102 : 0;
    return &texture;
}

void release(vita2d_texture* value) {
    // These are the ownership fields read by the real vita2d_free_texture().
    // An ordinary image must never destroy a target or free a depth buffer.
    assert(value == &texture && !value->gxm_rtgt && !value->depth_UID);
    assert(value->data_UID == 101);
    assert(value->palette_UID == (last_format == SCE_GXM_TEXTURE_FORMAT_P8_ABGR ? 102 : 0));
    unsigned char expected[sizeof(SceGxmTexture)];
    std::memset(expected, 0xA5, sizeof(expected));
    assert(!std::memcmp(&value->gxm_tex, expected, sizeof(expected)));
}
}

extern "C" {
vita2d_texture* __real_vita2d_create_empty_texture(unsigned width, unsigned height) {
    return allocate(width, height, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
}
vita2d_texture* __real_vita2d_create_empty_texture_format(unsigned width, unsigned height, SceGxmTextureFormat format) {
    return allocate(width, height, format);
}
}

int main() {
    for (int repeat = 0; repeat < 32; ++repeat) {
        release(__wrap_vita2d_create_empty_texture(32, 16));
        // PNG/RGBA images, glyph atlases, WebKit buffers and indexed palettes.
        for (auto format : {SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, SCE_GXM_TEXTURE_FORMAT_U8_R111,
                            SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB, SCE_GXM_TEXTURE_FORMAT_P8_ABGR}) {
            release(__wrap_vita2d_create_empty_texture_format(32, 16, format));
            assert(last_format == format);
        }
    }
    assert(calls == 160);
    allocation_failed = true;
    assert(!__wrap_vita2d_create_empty_texture(32, 16));
    assert(!__wrap_vita2d_create_empty_texture_format(32, 16, SCE_GXM_TEXTURE_FORMAT_U8_R111));
    assert(calls == 162);
    std::puts("Vita2d textures: reused heap, owned image/palette preservation and allocation failure PASS");
}
