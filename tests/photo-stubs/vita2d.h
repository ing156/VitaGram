#pragma once
struct vita2d_texture {};
inline float vita2d_texture_get_width(vita2d_texture*) { return 100; }
inline float vita2d_texture_get_height(vita2d_texture*) { return 100; }
inline void vita2d_enable_clipping() {}
inline void vita2d_disable_clipping() {}
inline void vita2d_set_clip_rectangle(int, int, int, int) {}
inline void vita2d_draw_texture_scale_rotate_hotspot(vita2d_texture*, float, float, float, float, float, float, float) {}
