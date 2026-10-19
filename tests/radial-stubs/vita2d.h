#pragma once
#include <cstddef>
struct vita2d_pgf {};
struct vita2d_texture;
struct vita2d_color_vertex { float x, y, z; unsigned color; };
constexpr int SCE_GXM_PRIMITIVE_TRIANGLE_STRIP = 6;
constexpr int SCE_GXM_PRIMITIVE_TRIANGLE_FAN = 5;
void* vita2d_pool_memalign(size_t, size_t);
void vita2d_draw_array(int, const vita2d_color_vertex*, size_t);
void vita2d_draw_rectangle(float, float, float, float, unsigned);
void vita2d_draw_fill_circle(float, float, float, unsigned);
