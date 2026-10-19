"""Resolve catalogue colors into UI roles and render the existing asset template."""
import argparse
import json
from pathlib import Path
import re
import shutil
import tempfile

ROLES = ('background', 'surface', 'text', 'muted', 'accent')
HEX = re.compile(r'#[0-9a-fA-F]{6}')


def rgb(color):
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


def blend(a, b, amount):
    return '#' + ''.join(f'{round(x * (1 - amount) + y * amount):02X}'
                         for x, y in zip(rgb(a), rgb(b)))


def luminance(color):
    values = (v / 255 for v in rgb(color))
    return sum(w * (v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4)
               for w, v in zip((.2126, .7152, .0722), values))


def contrast(a, b):
    low, high = sorted((luminance(a), luminance(b)))
    return (high + .05) / (low + .05)


def load_composition(path):
    document = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(document, dict) or set(document) != {'name', 'mode', 'catalog', 'colors'}:
        raise ValueError('composition needs exactly name, mode, catalog and colors')
    if not isinstance(document['name'], str) or not document['name'].strip():
        raise ValueError('name must be a non-empty string')
    if document['mode'] not in ('dark', 'light'):
        raise ValueError('mode must be dark or light')
    colors = document['colors']
    if not isinstance(colors, dict) or set(colors) != set(ROLES):
        raise ValueError('colors must assign: ' + ', '.join(ROLES))
    if not isinstance(document['catalog'], str) or not document['catalog'].strip():
        raise ValueError('catalog must be a path relative to the composition JSON')
    catalog = json.loads((path.parent / document['catalog']).read_text(encoding='utf-8'))
    if not isinstance(catalog, dict) or not isinstance(catalog.get('colors'), list):
        raise ValueError('catalog must contain a colors array')
    by_code = {}
    for entry in catalog['colors']:
        if (not isinstance(entry, dict) or not isinstance(entry.get('code'), str)
                or not entry['code'].strip() or not isinstance(entry.get('hex'), str)
                or not HEX.fullmatch(entry['hex'])):
            raise ValueError('catalog entries need a non-empty code and #RRGGBB hex')
        if entry['code'] in by_code:
            raise ValueError('duplicate catalog code: ' + entry['code'])
        by_code[entry['code']] = entry['hex'].upper()
    resolved = {}
    for role, code in colors.items():
        if not isinstance(code, str) or code not in by_code:
            raise ValueError(f'unknown catalog code for {role}: {code!r}')
        resolved[role] = by_code[code]
    p = make_palette(resolved, document['mode'])
    return document, p


def make_palette(colors, mode):
    p = dict(colors)
    dark = mode == 'dark'
    if (luminance(p['background']) < luminance(p['text'])) != dark:
        raise ValueError('background/text brightness does not match mode')
    p.update(
        header_top=p['accent'], header_bottom=blend(p['accent'], p['background'], .10),
        surface_secondary=blend(p['surface'], p['accent'], .08),
        hover=blend(p['surface'], p['accent'], .12),
        menu=p['surface'], selected=blend(p['surface'], p['accent'], .18),
        focus=p['accent'], divider=blend(p['surface'], p['text'], .18),
        border=blend(p['surface'], p['text'], .28),
        shadow=blend(p['background'], '#000000', .65),
        date=blend(p['surface'], p['accent'], .08),
        progress=blend(p['surface'], p['text'], .18),
        green='#23DB80' if dark else '#087448',
    )
    p['header_text'] = max((p['text'], p['background'], p['surface']),
                           key=lambda c: min(contrast(c, p[k]) for k in ('header_top', 'header_bottom')))
    # Reject unreadable role assignments before any output directory is created.
    for foreground in ('text', 'muted'):
        for background in ('background', 'surface', 'surface_secondary', 'selected', 'menu', 'date'):
            ratio = contrast(p[foreground], p[background])
            if ratio < 4.5:
                raise ValueError(f'{foreground}/{background} contrast {ratio:.2f}:1 is below 4.5:1')
    for background in ('header_top', 'header_bottom'):
        if contrast(p['header_text'], p[background]) < 4.5:
            raise ValueError('header needs an accent with stronger contrast to text or background')
    if contrast(p['accent'], p['surface']) < 3:
        raise ValueError('accent/surface contrast must be at least 3:1')
    return p


def wave_palette(p):
    return dict(
        base_top=p['background'], base_mid=p['background'],
        base_bottom=blend(p['background'], p['accent'], .10),
        halo=p['accent'], under=p['surface'], mid=blend(p['background'], p['accent'], .35),
        wave1=p['accent'], wave2=blend(p['background'], p['accent'], .60),
        ribbon=blend(p['accent'], p['text'], .20), highlight=p['text'],
        edge=p['text'], mist=p['accent'],
    )


def generate(composition, output, palette_only=False):
    document, palette = load_composition(composition)
    if output.exists():
        raise ValueError('output directory already exists; choose a new path')
    output.parent.mkdir(parents=True, exist_ok=True)
    # Publish only a complete pack; failures leave existing assets untouched.
    with tempfile.TemporaryDirectory(prefix='.vitagram-theme-', dir=output.parent) as work:
        root = Path(work) / 'pack'
        mode = document['mode']
        if not palette_only:
            from build_assets_source import build
            manifest = build(root, {mode: palette}, {mode: wave_palette(palette)})
        else:
            root.mkdir()
        shutil.copyfile(composition.parent / document['catalog'], root / 'catalog.json')
        document = dict(document, catalog='catalog.json')
        if not palette_only:
            manifest['composition'] = document
            (root / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        (root / 'composition.json').write_text(json.dumps(document, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        (root / 'palette.json').write_text(json.dumps(palette, indent=2) + '\n', encoding='utf-8')
        root.rename(output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('composition', type=Path)
    parser.add_argument('output', type=Path, help='New directory for PNGs, manifest and previews')
    parser.add_argument('--palette-only', action='store_true', help='Resolve colors only; reuse the runtime masks, no PNG generation')
    args = parser.parse_args()
    try:
        generate(args.composition, args.output, palette_only=args.palette_only)
    except (ValueError, OSError) as error:
        parser.exit(1, f'Error: {error}\n')
    print(args.output / ('palette.json' if args.palette_only else 'previews/comparison.png'))


if __name__ == '__main__':
    main()
