#pragma once
struct vita2d_texture;
constexpr int SCE_GXM_TEXTURE_FILTER_LINEAR = 1;
void vita2d_texture_set_filters(vita2d_texture*, int, int);
void vita2d_wait_rendering_done();
vita2d_texture* vita2d_create_empty_texture(unsigned, unsigned);
void* vita2d_texture_get_datap(vita2d_texture*);
unsigned vita2d_texture_get_stride(vita2d_texture*);
void vita2d_free_texture(vita2d_texture*);
