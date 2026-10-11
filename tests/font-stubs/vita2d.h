#pragma once
#include <vector>
struct vita2d_texture { unsigned width, height, stride; std::vector<unsigned char> pixels; };
constexpr unsigned SCE_GXM_TEXTURE_FORMAT_U8_R111 = 1;
constexpr unsigned SCE_GXM_TEXTURE_FILTER_POINT = 0;
vita2d_texture* vita2d_create_empty_texture_format(unsigned, unsigned, unsigned);
void* vita2d_texture_get_datap(vita2d_texture*);
unsigned vita2d_texture_get_stride(vita2d_texture*);
void vita2d_texture_set_filters(vita2d_texture*, unsigned, unsigned);
void vita2d_free_texture(vita2d_texture*);
void vita2d_draw_texture_tint_part_scale(const vita2d_texture*, float, float, float, float, float, float, float, float, unsigned);
