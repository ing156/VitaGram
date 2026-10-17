#pragma once
struct vita2d_pgf {};
#define RGBA8(r,g,b,a) (0u)
int vita2d_init();
int vita2d_fini();
vita2d_pgf* vita2d_load_default_pgf();
void vita2d_set_clear_color(unsigned);
void vita2d_start_drawing();
void vita2d_clear_screen();
void vita2d_end_drawing();
void vita2d_swap_buffers();
void vita2d_draw_rectangle(float, float, float, float, unsigned);
float vita2d_pgf_text_width(vita2d_pgf*, float, const char*);
void vita2d_pgf_draw_text(vita2d_pgf*, float, float, unsigned, float, const char*);
