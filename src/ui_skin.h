#pragma once
#include <vita2d.h>

// PNG skin for the decorative parts of the UI (VitaGram_UI_Assets v1.x). Paths, 9-slice insets and
// sprite rectangles come from app0:ui/manifest.json. Every draw call returns false when its asset
// is not loaded, so callers keep their original drawing as the fallback.
namespace ui_skin {

enum class Slice { Panel, Row, RowFocused, TabNormal, TabActive, Menu, MenuItemFocus, DateChip,
                   MediaFrame, FocusRing, ProgressTrack, ProgressFill, ScrollThumb, Dialog, Count };
enum class Image { Background, Header, ActiveIndicator, EdgeGlow, Count };
enum class Icon { File, Back, Next, Play, Pause, Search, Check, Close, Download, Send, Trash, Leave, Reply, Forward, ClearHistory, Count };

// Region of an atlas sprite plus the bounds of its visible (non-transparent) pixels.
struct Sprite {
    const vita2d_texture* texture = nullptr;
    float x = 0, y = 0, w = 0, h = 0;
    float vx = 0, vy = 0, vw = 0, vh = 0;   // visible bounds relative to the sprite
};

// Loads the textures of one theme, releasing the previous theme first. UI thread, outside a frame.
// Returns true when the manifest was read; individual missing files only disable their own element.
// Theme IDs: 0 = light, 1 = dark, 2 = VHBB.
bool load(int theme);
void unload();
// True when the last load() left the manifest, wallpaper or header missing and a retry is due.
bool needs_retry();
bool ready();
// True when the controller button sprites of the current theme are loaded.
bool has_buttons();

const unsigned int kWhite = 0xFFFFFFFFu;

// Shared opacity (percent) of card-like surfaces (rows, message cards); backgrounds only, never text.
void set_opacity(int percent);
// White (or `rgb`) tint carrying the shared opacity; the focused item is a little more solid.
unsigned int card_tint(bool focused, unsigned int rgb = 0x00FFFFFFu);

// Nine-slice at an arbitrary size. Corners shrink proportionally when the box is smaller than the
// insets. `tint` is an RGBA8 multiplier (use its alpha to fade).
bool nine(Slice slice, float x, float y, float w, float h, unsigned int tint = kWhite);
bool background(float y = 0, float height = 544);   // part of the full-screen wallpaper
bool header(float height = 36);                     // header strip scaled to `height`
// Covers the four corners of a rectangle with the surface color, leaving a rounded hole of `radius` (<= 8):
// rounds video frames without touching the video texture. Only valid over the flat media frame.
bool corners(float x, float y, float w, float h, float radius);
// Bottom-corner buttons (attach left, menu right) from assets/ui_extra/corner_buttons.png; `glow` 0..1 fades
// in the sparkle overlay. Theme independent.
bool corner_button(bool right, float glow, float opacity = 1.f);
// Subtle inward-fading scroll hint at a vertical edge; right mirrors the left edge.
bool edge_glow(bool right, float edge, float y, float height);
bool indicator(float x, float y, float w, float h);
bool icon(Icon icon, float x, float y, float size, unsigned int tint = kWhite);
// Controller button 0..8 in the order triangle, square, cross, circle, dpad, start, select, L, R.
bool button(int index, Sprite& sprite);
// Theme colors from the manifest; `fallback` is returned when the skin is not loaded.
unsigned int background_color(unsigned int fallback);
unsigned int surface_color(unsigned int fallback);

}  // namespace ui_skin
