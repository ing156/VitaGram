"""Regenerate offline CJK subsets (requires fontTools; normal HTML builds use checked-in WOFF files)."""
from pathlib import Path
import json
from fontTools import subset
root=Path(__file__).resolve().parents[1]
out=root/'tools/palette-editor/fonts';out.mkdir(exist_ok=True)
for code,family in [('ja','JP'),('ko','KR'),('zh-Hans','SC'),('zh-Hant','TC')]:
 text=(root/f'assets/lang/{code}.lang').read_text()+''.join(json.loads((root/f'tools/palette-editor/locales/{code}.json').read_text()).values())
 options=subset.Options();options.flavor='woff';options.name_IDs=['*'];options.name_legacy=True;options.name_languages=['*']
 font=subset.load_font(str(root/f'assets/NotoSans{family}-Regular.otf'),options)
 sub=subset.Subsetter(options=options);sub.populate(text=text);sub.subset(font)
 subset.save_font(font,str(out/f'{code}.woff'),options)
 print(code,(out/f'{code}.woff').stat().st_size)
