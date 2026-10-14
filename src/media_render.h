#pragma once
#include <vita2d.h>

// Called on the UI thread; uploads/evictions happen only between frames.
// Noto Sans for text + Noto Color Emoji as a color-glyph fallback.
bool rich_text_init();
// Adopts fallback/emoji fonts read in the background; call once per frame.
void rich_text_poll();
// Call after ui_skin::load(): switches controller icons between the themed atlas and the legacy PNGs.
void rich_text_skin_changed();
// Changes when available fonts change; text layout caches must be dropped.
unsigned rich_text_revision();
void rich_text_set_scale(float scale);
// Font for the interface language (Korean/Chinese), nullptr when the default fonts suffice.
// Safe before rich_text_init(); on the UI thread otherwise.
bool rich_text_set_language_font(const char* path);
enum class LanguageFontStatus { Loading, Ready, Failed };
// Queue only the latest request. Does not change the active face. UI thread only.
LanguageFontStatus rich_text_prepare_language_font(const char* path);
// Japanese uses the permanent fallback face, loaded by the startup worker.
LanguageFontStatus rich_text_japanese_font_status();
void emoji_load_one();
void rich_text_shutdown();
int rich_text_width(vita2d_pgf* font, float scale, const char* text);
void rich_text_draw(vita2d_pgf* font, int x, int baseline, unsigned int color,
                    float scale, const char* text);
vita2d_texture* load_webp(const char* path);
