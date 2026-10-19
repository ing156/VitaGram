"""Host checks: catalogue resolution, contrast and the real PNG template renderer."""
import json
import copy
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'assets/VitaGram_UI_Assets_v1'
sys.path.insert(0, str(ASSETS))
from generate_theme import contrast, generate, load_composition


class ThemeTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        self.catalog = {
            'colors': [dict(code=code, name='Same display name', hex=color) for code, color in (
                ('paper', '#F3F0E9'), ('stone', '#E8E4DB'), ('ink', '#383E46'),
                ('muted', '#3A413E'), ('blue', '#4A5B6B'),
                ('pale', '#D6D1C5'), ('green', '#8CC5AA'))]
        }
        self.document = dict(name='Test', mode='light', catalog='catalog.json', colors=dict(
            background='stone', surface='paper', text='ink', muted='muted', accent='blue'))
        self.path = self.root / 'composition.json'

    def write(self):
        (self.root / 'catalog.json').write_text(json.dumps(self.catalog))
        self.path.write_text(json.dumps(self.document))

    def test_resolve_by_code_and_contrast(self):
        self.write()
        document, palette = load_composition(self.path)
        self.assertEqual(palette['surface'], '#F3F0E9')
        self.assertEqual(document['colors']['text'], 'ink')
        self.assertGreaterEqual(contrast(palette['muted'], palette['selected']), 4.5)
        self.assertAlmostEqual(contrast('#000000', '#FFFFFF'), 21)

    def test_invalid_inputs_leave_no_output(self):
        original_catalog = copy.deepcopy(self.catalog)
        original_document = copy.deepcopy(self.document)
        for change in ('missing', 'unknown', 'low_contrast', 'bad_hex', 'duplicate', 'bad_mode'):
            with self.subTest(change=change):
                self.catalog = copy.deepcopy(original_catalog)
                self.document = copy.deepcopy(original_document)
                if change == 'missing':
                    del self.document['colors']['surface']
                elif change == 'unknown':
                    self.document['colors']['accent'] = 'does not exist'
                elif change == 'low_contrast':
                    self.document['colors']['text'] = 'paper'
                elif change == 'bad_hex':
                    self.catalog['colors'][0]['hex'] = '#XYZXYZ'
                elif change == 'duplicate':
                    self.catalog['colors'].append(self.catalog['colors'][0])
                else:
                    self.document['mode'] = 'sepia'
                self.write()
                with self.assertRaises(ValueError):
                    generate(self.path, self.root / 'output')
                self.assertFalse((self.root / 'output').exists())

    def test_render_both_modes(self):
        from PIL import Image, ImageChops
        source = json.loads((ASSETS / 'manifest.json').read_text())
        for mode in ('light', 'dark'):
            with self.subTest(mode=mode):
                if mode == 'dark':
                    self.document.update(mode='dark', colors=dict(background='muted', surface='ink',
                                         text='paper', muted='pale', accent='green'))
                self.write()
                output = generate(self.path, self.root / mode)
                manifest = json.loads((output / 'manifest.json').read_text())
                palette = manifest['themes'][mode]
                for section in ('nine_slice', 'textures', 'sprite_atlases'):
                    for key, entry in manifest[section].items():
                        with Image.open(output / key) as image:
                            self.assertEqual(image.mode, 'RGBA')
                            self.assertEqual(list(image.size), entry['size'])
                        if section == 'nine_slice':
                            self.assertEqual(entry['insets'], source[section][key]['insets'])
                        if section == 'sprite_atlases':
                            self.assertEqual(entry['sprites'], source[section][key]['sprites'])
                with Image.open(output / f'previews/{mode}_list_960x544.png') as preview:
                    self.assertEqual(preview.size, (960, 544))
                key = f'themes/{mode}/icons/clear-history.png'
                self.assertIsNone(ImageChops.difference(Image.open(ASSETS / key).getchannel('A'),
                                                       Image.open(output / key).getchannel('A')).getbbox())
                key = f'themes/{mode}/9slice/row.png'
                self.assertEqual(Image.open(output / key).getpixel((24, 24))[:3],
                                 tuple(bytes.fromhex(palette['surface'][1:])))
                with self.assertRaisesRegex(ValueError, 'already exists'):
                    generate(self.path, output)
                self.assertEqual(json.loads((output / 'manifest.json').read_text()), manifest)
                self.assertEqual(load_composition(output / 'composition.json')[1], palette)

    def test_palette_only_and_shared_masks(self):
        from PIL import Image, ImageChops
        self.write()
        output = generate(self.path, self.root / 'colors', palette_only=True)
        self.assertFalse(list(output.rglob('*.png')))
        self.assertEqual(load_composition(output / 'composition.json')[1],
                         json.loads((output / 'palette.json').read_text()))
        manifest = json.loads((ASSETS / 'masks/manifest.json').read_text())
        roles = manifest['palettes']['dark']
        for name, entry in manifest['assets'].items():
            width, height = entry['size']
            self.assertTrue(entry['layers'], name)
            for layer in entry['layers']:
                self.assertIn(layer['role'], roles)
                x, y, w, h = layer['rect']
                self.assertTrue(x >= 0 and y >= 0 and w > 0 and h > 0 and x + w <= width and y + h <= height)
                with Image.open(ASSETS / layer['mask']) as image:
                    self.assertEqual(image.size, (w, h))
                    self.assertEqual(image.mode, 'RGBA')
                    self.assertEqual(image.getextrema()[:3], ((255, 255),) * 3)
            if name.startswith('icons/'):
                alpha = Image.new('L', (width, height))
                for layer in entry['layers']:
                    x, y, w, h = layer['rect']
                    with Image.open(ASSETS / layer['mask']) as image:
                        plane = Image.new('L', (width, height))
                        plane.paste(image.getchannel('A'), (x, y))
                        alpha = ImageChops.lighter(alpha, plane)
                with Image.open(ASSETS / 'themes/dark' / name) as original:
                    self.assertIsNone(ImageChops.difference(alpha, original.getchannel('A')).getbbox(), name)


if __name__ == '__main__':
    unittest.main()
