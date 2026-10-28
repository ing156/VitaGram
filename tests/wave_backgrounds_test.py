"""VHBB keeps the shared wave geometry and ships both palette variants."""
from pathlib import Path
import sys
import unittest
import xml.etree.ElementTree as ET

from PIL import Image

ASSETS = Path(__file__).resolve().parents[1] / 'assets/VitaGram_UI_Assets_v1'
sys.path.insert(0, str(ASSETS))
from generate_wave_backgrounds import PALETTES, svg_wallpaper, wallpaper


class WaveBackgroundTest(unittest.TestCase):
    def test_shared_geometry(self):
        for alt in (False, True):
            trees = [ET.fromstring(svg_wallpaper(theme, alt)) for theme in ('dark', 'vhbb')]
            for tree in trees:
                for node in tree.iter():
                    for key in ('stop-color', 'fill', 'stroke'):
                        if node.get(key, '').startswith('#'):
                            node.set(key, 'palette')
            self.assertEqual(ET.tostring(trees[0]), ET.tostring(trees[1]))
        self.assertEqual(PALETTES['vhbb']['base_mid'], '#12246B')
        self.assertNotEqual(PALETTES['dark'], PALETTES['vhbb'])

    def test_packaged_wallpapers(self):
        variants = []
        for alt, name in enumerate(('background_960x544.png', 'background_alt_960x544.png')):
            with Image.open(ASSETS / 'themes/vhbb/fixed' / name) as image:
                self.assertEqual(image.size, (960, 544))
                self.assertEqual(image.mode, 'RGBA')
                self.assertEqual(image.getchannel('A').getextrema(), (255, 255))
                self.assertEqual(image.tobytes(), wallpaper('vhbb', bool(alt)).tobytes())
                variants.append(image.tobytes())
        self.assertNotEqual(*variants)


if __name__ == '__main__':
    unittest.main()
