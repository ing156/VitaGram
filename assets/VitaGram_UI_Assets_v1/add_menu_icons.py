"""Update only menu glyphs, preserving existing atlas sprites and wallpapers."""
import json
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
manifest_path = ROOT / 'manifest.json'
manifest = json.loads(manifest_path.read_text())
for theme in ('dark', 'light'):
    atlas_path = f'themes/{theme}/ui_icons_atlas.png'
    data = manifest['sprite_atlases'][atlas_path]
    original = Image.open(ROOT / atlas_path).convert('RGBA')
    atlas = Image.new('RGBA', (512, max(384, original.height)))
    atlas.alpha_composite(original)
    atlas.paste((0, 0, 0, 0), (208, 336, 240, 368))
    for index, name in enumerate(('trash', 'leave', 'reply', 'clear-history', 'forward')):
        image = Image.new('RGBA', (128, 128))
        draw = ImageDraw.Draw(image)
        color = '#FA6078' if name == 'leave' and theme == 'dark' else '#D93751' if name == 'leave' else '#F2F7FF' if theme == 'dark' else '#102A50'
        def line(points):
            points = [(round(x * 128 / 24), round(y * 128 / 24)) for x, y in points]
            draw.line(points, fill=color, width=10, joint='curve')
            for x, y in points:
                draw.ellipse((x-4, y-4, x+4, y+4), fill=color)
        if name == 'trash':
            line([(4, 6), (20, 6)])
            line([(9, 6), (9, 3), (15, 3), (15, 6)])
            line([(6, 6), (7, 21), (17, 21), (18, 6)])
            line([(10, 10), (10, 17)])
            line([(14, 10), (14, 17)])
        elif name == 'leave':
            line([(13, 7), (13, 4), (3, 4), (3, 20), (13, 20), (13, 17)])
            line([(9, 12), (22, 12)])
            line([(18, 8), (22, 12), (18, 16)])
        elif name == 'clear-history':
            line([(11, 12), (20, 3)])
            line([(10, 11), (14, 15), (10, 21), (4, 19), (3, 17), (10, 11)])
            line([(8, 15), (5, 18)])
            line([(10, 16), (8, 20)])
        else:
            # Bent outline arrow, reflected for forwarding.
            points = [(10, 4), (3, 11), (10, 18), (10, 13), (13, 13),
                      (16, 14), (19, 17), (21, 21), (21, 16), (20, 12),
                      (18, 9), (15, 7), (10, 7), (10, 4)]
            if name == 'forward':
                points = [(24-x, y) for x, y in points]
            line(points)
        icon = image.resize((32, 32), Image.Resampling.LANCZOS)
        icon.save(ROOT / f'themes/{theme}/icons/{name}.png')
        x, y = (16, 144) if name == 'forward' else (16 + index * 64, 336)
        atlas.paste((0, 0, 0, 0), (x, y, x+32, y+32))
        atlas.paste(icon, (x, y))
        data['sprites'][name] = dict(x=x, y=y, w=32, h=32)
    data['size'] = list(atlas.size)
    atlas.save(ROOT / atlas_path)
manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
