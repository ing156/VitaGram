#pragma once
#include <vector>
#define RGBA8(r,g,b,a) ((unsigned)(r) | ((unsigned)(g)<<8) | ((unsigned)(b)<<16) | ((unsigned)(a)<<24))
struct vita2d_texture { unsigned width, height, stride; std::vector<unsigned char> pixels; };
constexpr unsigned SCE_GXM_TEXTURE_FILTER_POINT = 0, SCE_GXM_TEXTURE_FILTER_LINEAR = 1;
vita2d_texture* vita2d_create_empty_texture(unsigned, unsigned);
vita2d_texture* vita2d_load_PNG_buffer(const void*);
void* vita2d_texture_get_datap(const vita2d_texture*);
unsigned vita2d_texture_get_stride(const vita2d_texture*);
unsigned vita2d_texture_get_width(const vita2d_texture*);
unsigned vita2d_texture_get_height(const vita2d_texture*);
void vita2d_texture_set_filters(vita2d_texture*, unsigned, unsigned);
void vita2d_free_texture(vita2d_texture*);
void vita2d_wait_rendering_done();
void vita2d_draw_texture_part_scale(const vita2d_texture*, float,float,float,float,float,float,float,float);
void vita2d_draw_texture_tint_part_scale(const vita2d_texture*, float,float,float,float,float,float,float,float,unsigned);
void vita2d_draw_texture_part(const vita2d_texture*, float,float,float,float,float,float);
void vita2d_draw_texture_scale(const vita2d_texture*, float,float,float,float);
void vita2d_draw_rectangle(float,float,float,float,unsigned);
