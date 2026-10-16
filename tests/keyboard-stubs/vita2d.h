#pragma once
struct vita2d_pgf {};
struct vita2d_texture {};
#define RGBA8(r,g,b,a) ((unsigned)(r) | ((unsigned)(g)<<8) | ((unsigned)(b)<<16) | ((unsigned)(a)<<24))
void vita2d_draw_rectangle(float, float, float, float, unsigned);
void vita2d_draw_fill_circle(float, float, float, unsigned);
void vita2d_draw_line(float, float, float, float, unsigned);
void vita2d_draw_texture_tint_part_scale(const vita2d_texture*, float, float, float, float, float, float, float, float, unsigned);

void vita2d_wait_rendering_done();
int sceGxmDisplayQueueFinish();
