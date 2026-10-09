# Battery sprites (v1.2)

Six 32x32 RGBA status icons per theme: `battery_0`, `battery_25`, `battery_50`, `battery_75`, `battery_100`, `battery_charging`.

- Standalone PNG: `themes/{dark,light}/icons/<sprite>.png`.
- Atlas: `themes/{dark,light}/ui_icons_atlas.png` (512x320, **same size as v1.1**).
- Sprite coordinates: `manifest.json > sprite_atlases > ... > sprites`. Existing 34 icons retain original positions.
- Suggested display size: 20x20 px in the PS Vita header; preserve alpha.
- Choose state from live battery percentage; `battery_charging` takes priority when charging. Use `manifest.json > battery_icons > display_rules` as optional thresholds.
- Never hardcode charge values. No behavior or battery telemetry is included in this asset pack.

Example: load atlas once, fetch `battery_75` source rectangle from manifest/static constants, draw with `vita2d_draw_texture_part_scale(atlas, x, y, src_x, src_y, 32, 32, 20.f/32.f, 20.f/32.f)`.
