"""Build one reusable set of white/alpha UI masks; palette colors stay in JSON."""
import argparse
import copy
import io
import json
from pathlib import Path
import tempfile
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw
import cairosvg

from build_assets_source import build
from generate_wave_backgrounds import svg_wallpaper

ROOT = Path(__file__).resolve().parent
EXTRA_COLORS = dict(header_text='#F2F7FF', highlight='#FFFFFF', danger='#F36B85',
                    warning='#F5B357', button_cross='#26B2FF', button_circle='#FA6078',
                    button_square='#F27CCC', button_triangle='#39D990',
                    battery_low='#F36B85', battery_warning='#F5B357', battery_charging='#23DB80')


class Masks:
    def __init__(self, root):
        self.root = root
        self.entries = {}

    def save(self, name, metadata, layers):
        entry = {key: value for key, value in metadata.items() if key in ('size', 'insets', 'sprites', 'cell')}
        entry['layers'] = []
        for index, (role, alpha) in enumerate(layers):
            box = alpha.getbbox()
            if box is None:
                continue
            path = Path(name).with_suffix('') / f'{index:02d}-{role}.png'
            image = Image.new('RGBA', (box[2] - box[0], box[3] - box[1]), 'white')
            image.putalpha(alpha.crop(box))
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            image.save(target, optimize=True)
            entry['layers'].append(dict(role=role, mask='masks/' + path.as_posix(),
                                        rect=[box[0], box[1], image.width, image.height]))
        self.entries[name] = entry


def solid_alpha(size, value=255):
    return Image.new('L', tuple(size), value)


def split_colors(image, colors):
    """Keep original glyph coverage, separating its existing semantic color regions."""
    masks = {role: Image.new('L', image.size) for role in colors}
    pixels = {role: mask.load() for role, mask in masks.items()}
    rgb = {role: tuple(bytes.fromhex(color[1:])) for role, color in colors.items()}
    for y in range(image.height):
        for x in range(image.width):
            r, g, b, a = image.getpixel((x, y))
            if a:
                role = min(rgb, key=lambda k: sum((c - v) ** 2 for c, v in zip((r, g, b), rgb[k])))
                pixels[role][x, y] = a
    return list(masks.items())


def background_layers(alternate):
    # Preserve the existing SVG geometry and opacity gradients; each shape gets a color role.
    svg = ET.fromstring(svg_wallpaper('dark', alternate))
    for node in svg.iter():
        for key in ('fill', 'stroke', 'stop-color'):
            if node.get(key, '').startswith('#'):
                node.set(key, '#FFFFFF')
    children = list(svg)
    defs = children[0]
    roles = ['background', 'accent', 'accent', 'accent', 'focus', 'surface_secondary',
             'accent', 'focus', 'focus', 'focus', 'accent', 'focus']
    shapes = children[1:]
    assert len(shapes) == len(roles)
    result = []
    for shape, role in zip(shapes, roles):
        if not result:
            result.append((role, solid_alpha((960, 544))))
            continue
        layer = ET.Element(svg.tag, svg.attrib)
        layer.append(copy.deepcopy(defs))
        layer.append(copy.deepcopy(shape))
        raw = cairosvg.svg2png(bytestring=ET.tostring(layer), output_width=1920, output_height=1088)
        alpha = Image.open(io.BytesIO(raw)).convert('RGBA').resize((960, 544), Image.Resampling.LANCZOS).getchannel('A')
        result.append((role, alpha))
    return result


def generate(output):
    if output.exists():
        raise ValueError('output already exists; choose a new directory')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.vitagram-masks-', dir=output.parent) as work:
        root = Path(work) / 'masks'
        masks = Masks(root)
        source = json.loads((ROOT / 'manifest.json').read_text())
        palettes = {mode: dict(source['themes'][mode], **EXTRA_COLORS) for mode in ('light', 'dark')}
        for palette in palettes.values():
            palette.update(icon=palette['text'], notification='#1E90FF', control_active=palette['accent'],
                           control_inactive=palette['muted'], delivery=palette['green'])
        # Shapes are shared by all palettes. Render once to extract nine-slice masks.
        build(Path(work) / 'reference', {'dark': source['themes']['dark']}, emit_masks=masks.save)
        for alt in (False, True):
            name = 'fixed/background' + ('_alt' if alt else '') + '_960x544.png'
            masks.save(name, dict(size=[960, 544]), background_layers(alt))
        for name, height, top, bottom in (('header_960x40', 40, 'header_top', 'header_bottom'),
                                           ('footer_960x44', 44, 'header_bottom', 'header_top')):
            gradient = Image.new('L', (960, height))
            draw = ImageDraw.Draw(gradient)
            for y in range(height):
                draw.line((0, y, 959, y), fill=round(255 * (1 - y / (height - 1))))
            border = Image.new('L', gradient.size)
            ImageDraw.Draw(border).line((0, height - 1, 959, height - 1), fill=225)
            masks.save(f'fixed/{name}.png', dict(size=[960, height]),
                       [(bottom, solid_alpha(gradient.size)), (top, gradient), ('border', border)])
        for name, role in (('active_indicator_128x4.png', 'accent'), ('focus_edge_glow_16x128.png', 'accent')):
            with Image.open(ROOT / 'themes/dark/fixed' / name) as image:
                masks.save('fixed/' + name, dict(size=list(image.size)), [(role, image.getchannel('A'))])

        for atlas_name in ('ui_icons_atlas.png', 'ps_buttons_atlas.png'):
            meta = source['sprite_atlases']['themes/dark/' + atlas_name]
            atlas = Image.open(ROOT / 'themes/dark' / atlas_name).convert('RGBA')
            layers = []
            for name, rect in meta['sprites'].items():
                box = (rect['x'], rect['y'], rect['x'] + rect['w'], rect['y'] + rect['h'])
                sprite = atlas.crop(box)
                if atlas_name.startswith('ps_'):
                    colors = {k: palettes['dark'][k] for k in ('text', 'background', 'surface_secondary', 'border')}
                    colors.update({k: v for k, v in EXTRA_COLORS.items() if k.startswith('button_')})
                    directory = 'buttons'
                else:
                    colors = dict(icon=palettes['dark']['text'])
                    if name == 'leave':
                        colors = dict(danger='#FA6078')
                    if name.startswith('battery_'):
                        colors = dict(header_text='#F2F7FF', danger='#F36B85', warning='#F5B357',
                                      focus='#36BAFF', green='#30E08A')
                    directory = 'icons'
                pieces = split_colors(sprite, colors)
                if name.startswith('battery_'):
                    roles = dict(focus='header_text', danger='battery_low', warning='battery_warning', green='battery_charging')
                    pieces = [(roles.get(role, role), alpha) for role, alpha in pieces]
                masks.save(f'{directory}/{name}.png', dict(size=list(sprite.size)), pieces)
                for role, alpha in pieces:
                    plane = Image.new('L', atlas.size)
                    plane.paste(alpha, (rect['x'], rect['y']))
                    layers.append((role, plane))
            masks.save(atlas_name, meta, layers)

        # The corner-button source is already neutral. Separate its dark body and pale highlights.
        corner = Image.open(ROOT.parent / 'ui_extra/corner_buttons.png').convert('RGBA')
        body, highlight = Image.new('L', corner.size), Image.new('L', corner.size)
        for y in range(corner.height):
            for x in range(corner.width):
                r, g, b, a = corner.getpixel((x, y))
                weight = max(0, min(1, ((r + g + b) / 3 - 34) / (255 - 34)))
                front = a / 255 * weight
                highlight.putpixel((x, y), round(front * 255))
                body.putpixel((x, y), round(a * (1 - weight) / (1 - front)) if front < 1 else 0)
        masks.save('corner_buttons.png', dict(size=list(corner.size)), [('surface', body), ('icon', highlight)])
        manifest = dict(version=1, palettes=palettes, assets=masks.entries)
        (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        root.rename(output)
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        manifest = generate(args.output)
    except (ValueError, OSError) as error:
        parser.exit(1, f'Error: {error}\n')
    print(f'{len(manifest["assets"])} elements: {args.output}')
