#pragma once
#include <vita2d.h>

// Called on the UI thread; uploads/evictions happen only between frames.
// Noto Sans for text + Noto Color Emoji as a color-glyph fallback.
bool rich_text_init();
// Adopts fallback/emoji fonts read in the background; call once per frame.
void rich_text_poll();
// Changes when available fonts change; text layout caches must be dropped.
unsigned rich_text_revision();
void rich_text_set_scale(float scale);
void emoji_load_one();
void rich_text_shutdown();
int rich_text_width(vita2d_pgf* font, float scale, const char* text);
void rich_text_draw(vita2d_pgf* font, int x, int baseline, unsigned int color,
                    float scale, const char* text);
vita2d_texture* load_webp(const char* path);
