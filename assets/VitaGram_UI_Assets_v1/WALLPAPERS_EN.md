# VitaGram UI v1.1 — PS Vita-inspired wave wallpapers

The default files `themes/{dark,light,vhbb}/fixed/background_960x544.png` have been replaced with new static wave-pattern wallpapers. Alternative wallpapers with stronger wave saturation are available at `themes/{dark,light,vhbb}/fixed/background_alt_960x544.png`.

All six wallpapers are 960x544, RGBA PNG. They are intentionally text-free and screen-sized. Each renders with a **single** `vita2d_draw_texture(wallpaper, 0, 0)` call (no animation, shaders, or runtime geometry required). Keep existing content, labels, focus, and dynamic tabs rendered above the wallpaper.

Recommended: use the default wallpaper for most chat/message screens; use the alternative only if the extra wave contrast does not conflict with readability. Existing asset paths, 9-slice parameters and sprite atlas layouts have not changed. The manifest registers both wallpapers under `textures`.

The wallpaper renderer is reproducible with `generate_wave_backgrounds.py` (offline only, needs Python, Pillow and CairoSVG). Do not include this script in a VPK. These are original vector-generated PS Vita-inspired designs, not extracted console system wallpaper resources.

VHBB uses the same wave geometry with its own blue/violet palette: base `#12246B`, violet `#50448D`, blue `#117BDD` and highlights `#95D6FF`. Regenerate its wallpapers and preview with `generate_vhbb_theme.py` (also needs CairoSVG).

### Example

```cpp
// Load once during initialization, not once per frame:
// vita2d_texture *wallpaper = vita2d_load_PNG_file(
//     "app0:assets/vitagram_ui/themes/dark/fixed/background_960x544.png");
// Then draw at the start of your normal 960x544 frame:
// vita2d_draw_texture(wallpaper, 0.0f, 0.0f);
```
