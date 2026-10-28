#include <vita2d.h>

namespace {
vita2d_texture* ordinary_texture(vita2d_texture* texture) {
    // vita2d leaves these uninitialized, then frees them as owned GPU resources.
    if (texture) {
        texture->gxm_rtgt = nullptr;
        texture->depth_UID = 0;
    }
    return texture;
}
}

extern "C" {
vita2d_texture* __real_vita2d_create_empty_texture(unsigned int, unsigned int);
vita2d_texture* __real_vita2d_create_empty_texture_format(unsigned int, unsigned int, SceGxmTextureFormat);

vita2d_texture* __wrap_vita2d_create_empty_texture(unsigned int width, unsigned int height) {
    return ordinary_texture(__real_vita2d_create_empty_texture(width, height));
}

vita2d_texture* __wrap_vita2d_create_empty_texture_format(unsigned int width, unsigned int height, SceGxmTextureFormat format) {
    return ordinary_texture(__real_vita2d_create_empty_texture_format(width, height, format));
}
}
