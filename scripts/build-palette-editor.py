"""Pack the local palette editor into one offline HTML application (standard library only)."""
import argparse
import base64
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'assets/VitaGram_UI_Assets_v1'


def build(output):
    manifest = json.loads((ASSETS / 'masks/manifest.json').read_text())
    paths = {layer['mask'] for entry in manifest['assets'].values() for layer in entry['layers']}
    images = {path: base64.b64encode((ASSETS / path).read_bytes()).decode('ascii') for path in sorted(paths)}
    presets = [{'name': 'Светлая', 'colors': manifest['palettes']['light']},
               {'name': 'Тёмная', 'colors': manifest['palettes']['dark']}]
    for name in ('little-greene-light','little-greene-dark'):
        presets.append({'name':name,'colors':json.loads((ASSETS/'palettes'/(name+'.json')).read_text())})
    for name in ('black-blue','black-red'):
        presets.append({'name':name,'colors':json.loads((ASSETS/'palettes'/name/'palette.json').read_text())})
    source=json.loads((ROOT/'assets/open-color.json').read_text())
    catalogs=[{'name':source['brand'],'note':source['note'],'license':source['license'],
               'source':source.get('source',''),'colors':source['colors']}]
    source=json.loads((ROOT/'assets/little-greene.json').read_text())
    catalogs.append({'name':source['brand'],'note':source.get('note',''),
                     'source':source.get('source',''),
                     'colors':[{key:color[key] for key in ('code','name','hex')} for color in source['colors']]})
    layout = {name: int(value) for name, value in re.findall(r'inline constexpr int (\w+) = (\d+);',
              (ROOT / 'src/ui_layout.h').read_text())}
    layout['font_base'] = int(re.search(r'kBaseFontSizePx = (\d+);', (ROOT / 'src/media_render.cpp').read_text())[1])
    version = re.search(r'set\(VITAGRAM_VERSION "([\d.]+)"\)', (ROOT / 'CMakeLists.txt').read_text())[1]
    languages = {}
    for path in sorted((ROOT/'assets/lang').glob('*.lang')):
        language = path.stem
        strings = {}
        for line in path.read_text().splitlines():
            if '=' in line and not line.lstrip().startswith(('#', '@')):
                key, value = line.split('=', 1)
                value = value.strip()
                if len(value) >= 2 and value[0] == value[-1] == '"':
                    value = value[1:-1]
                strings[key.strip()] = re.sub(r'\\(.)', lambda match: '\n' if match[1] == 'n' else match[1], value)
        languages[language] = strings
    strings = languages['ru']
    font = base64.b64encode((ROOT / 'assets/NotoSans-Regular.ttf').read_bytes()).decode('ascii')
    themes = []
    source = json.loads((ASSETS / 'manifest.json').read_text())
    for brand, mode, filename in re.findall(r'\{"([^"]*)", ([012]), (?:nullptr|"([^"]+)")\}',
                                           (ROOT / 'src/ui_themes.h').read_text()):
        base = ('light', 'dark', 'vhbb')[int(mode)]
        colors = json.loads((ASSETS / 'palettes' / filename).read_text()) if filename else source['themes'][base]
        name = (brand + ' / ' if brand else '') + strings['settings.theme_' + base]
        themes.append({'name': name, 'brand': brand, 'base': base, 'colors': colors})
    radial_source = (ROOT / 'src/radial_menu.h').read_text().split('struct Context')[0]
    radial = {name: float(value) for name, value in re.findall(r'(\w+) = (\d+(?:\.\d+)?)(?=[,;])', radial_source)}
    data = {'manifest': manifest, 'images': images, 'presets': presets, 'catalogs': catalogs,
            'layout': layout, 'radial': radial, 'version': version, 'strings': strings, 'font': font,
            'themes': themes, 'languages': languages,
            'translations': {path.stem:json.loads(path.read_text()) for path in (ROOT/'tools/palette-editor/locales').glob('*.json')},
            'language_names': {code: re.search(r'^@name\s*=\s*(.+)$', (ROOT/f'assets/lang/{code}.lang').read_text(), re.M)[1] for code in languages},
            'cjk_fonts': {code:base64.b64encode((ROOT/f'tools/palette-editor/fonts/{code}.woff').read_bytes()).decode('ascii') for code in ('ja','ko','zh-Hans','zh-Hant')},
            'cjk_license': (ROOT/'assets/NotoSansJP-LICENSE.txt').read_text(),
            'font_license': (ROOT / 'assets/NotoSans-LICENSE.txt').read_text(),
            'app_license': (ROOT / 'LICENSE').read_text(),
            'catalog_license': (ROOT/'assets/OpenColor-LICENSE.txt').read_text()}
    template = ROOT / 'tools/palette-editor'
    html = (template / 'editor.html').read_text()
    replacements = {'__DATA__': json.dumps(data, ensure_ascii=True, separators=(',', ':')).replace('<', '\\u003c'),
                    '__I18N__': (template / 'i18n.js').read_text(),
                    '__CORE__': (template / 'palette-core.js').read_text(),
                    '__PREVIEW__': (template / 'native-preview.js').read_text(),
                    '__EDITOR__': (template / 'editor.js').read_text()}
    for marker, value in replacements.items():
        assert html.count(marker) == 1, marker
        html = html.replace(marker, value)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix('.tmp')
    temporary.write_text(html, encoding='utf-8')
    temporary.replace(output)
    return data


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', nargs='?', type=Path,
                        default=ROOT / 'build-modern/palette-editor/VitaGram Palette Editor.html')
    args = parser.parse_args()
    data = build(args.output)
    print(f'{args.output}: {len(data["presets"])} patterns, {len(data["catalogs"])} catalogs, offline')
