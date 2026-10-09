// VitaGram UI PNG assets: minimal draw helpers for an existing vita2d renderer.
// Source reference: libvita2d/include/vita2d.h (check your own fork).
// Example intentionally does NOT change your input or screen architecture.
#include <vita2d.h>

struct NineSlice { int left, top, right, bottom; };

static void draw_9slice(const vita2d_texture *tex,
                        float x, float y, float w, float h,
                        NineSlice m) {
    if (!tex || w < m.left+m.right || h < m.top+m.bottom) return;
    const float tw = (float)vita2d_texture_get_width(tex);
    const float th = (float)vita2d_texture_get_height(tex);
    const float sw[3] = { (float)m.left, tw-m.left-m.right, (float)m.right };
    const float sh[3] = { (float)m.top, th-m.top-m.bottom, (float)m.bottom };
    const float dw[3] = { (float)m.left, w-m.left-m.right, (float)m.right };
    const float dh[3] = { (float)m.top, h-m.top-m.bottom, (float)m.bottom };
    float sy=0, dy=y;
    for (int row=0; row<3; ++row) {
        float sx=0, dx=x;
        for (int col=0; col<3; ++col) {
            if (sw[col] > 0 && sh[row] > 0 && dw[col] > 0 && dh[row] > 0)
                vita2d_draw_texture_part_scale(tex, dx, dy, sx, sy,
                    sw[col], sh[row], dw[col]/sw[col], dh[row]/sh[row]);
            sx+=sw[col]; dx+=dw[col];
        }
        sy+=sh[row]; dy+=dh[row];
    }
}

// Load ONCE during initialization; paths reflect your installed resource folder:
// vita2d_texture *row = vita2d_load_PNG_file("app0:assets/dark/row.png");
// Draw as an ordinary rectangle panel with rounded corners from the image:
// draw_9slice(row, 18, 82, 924, 48, {12,12,12,12});
// Do not load PNGs in the frame loop. Free with vita2d_free_texture() at exit.
//
// For atlas icons, manifest.json gives the precise source x/y/width/height.
// Example 32px icon from a themed atlas (no resizing):
// vita2d_draw_texture_part(ui_icons, dst_x, dst_y, src_x, src_y, 32, 32);
// For a 48px Vita button from buttons atlas:
// vita2d_draw_texture_part(buttons, dst_x, dst_y, src_x, src_y, 48, 48);
// PNGs use straight RGBA8 and transparent pixels. Check blending enabled by your build.
