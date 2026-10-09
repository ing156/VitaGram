"""Generate original Vita-inspired wave wallpapers for VitaGram 960x544.

Usage: python generate_wave_backgrounds.py /path/to/VitaGram_UI_Assets_v1
No external copyrighted PS Vita wallpapers are embedded. Uses vector SVG paths,
linear/radial gradients and 2x supersampling; ships finished PNGs for vita2d.
"""
from __future__ import annotations
import io, sys
from pathlib import Path
from PIL import Image
import cairosvg

W, H = 960, 544

PALETTES = {
  'dark': {
    'base_top':'#07162E','base_mid':'#071D3C','base_bottom':'#0A2857',
    'halo':'#1B75CF','under':'#0A367A','mid':'#1364BB',
    'wave1':'#1682E2','wave2':'#0B51AC', 'ribbon':'#51AAFC',
    'highlight':'#7ACBFF', 'edge':'#B7E6FF', 'mist':'#185CBD'
  },
  'light': {
    'base_top':'#F7FBFF','base_mid':'#E8F5FF','base_bottom':'#D5EAFD',
    'halo':'#A6DBFF','under':'#94C6F2','mid':'#73B4EF',
    'wave1':'#3B9AEA','wave2':'#8AC9F8', 'ribbon':'#B7E4FF',
    'highlight':'#FFFFFF', 'edge':'#FFFFFF', 'mist':'#C7E6FF'
  }
}

def svg_wallpaper(theme: str, alt: bool = False) -> str:
  c=PALETTES[theme]
  b=1.6 if alt else 1.0
  # Base layers remain quiet behind messages; waves concentrated in lower 2/3.
  dark = theme=='dark'
  def op(v): return f'{min(v*b, 1):.3f}'
  svg=f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 960 544" width="960" height="544">
<defs>
 <linearGradient id="base" x1="0" y1="0" x2="0.82" y2="1">
  <stop offset="0" stop-color="{c['base_top']}"/>
  <stop offset="0.54" stop-color="{c['base_mid']}"/>
  <stop offset="1" stop-color="{c['base_bottom']}"/>
 </linearGradient>
 <radialGradient id="glow" cx="85%" cy="50%" r="75%">
  <stop offset="0" stop-color="{c['halo']}" stop-opacity="{op(.25 if dark else .32)}"/>
  <stop offset=".53" stop-color="{c['halo']}" stop-opacity="{op(.08 if dark else .14)}"/>
  <stop offset="1" stop-color="{c['halo']}" stop-opacity="0"/>
 </radialGradient>
 <linearGradient id="far" x1="0" y1=".85" x2="1" y2=".18">
  <stop offset="0" stop-color="{c['under']}" stop-opacity="{op(.32 if dark else .26)}"/>
  <stop offset=".52" stop-color="{c['mid']}" stop-opacity="{op(.32 if dark else .30)}"/>
  <stop offset="1" stop-color="{c['wave1']}" stop-opacity="{op(.57 if dark else .42)}"/>
 </linearGradient>
 <linearGradient id="main" x1="0" y1="1" x2="1" y2="0">
  <stop offset="0" stop-color="{c['mid']}" stop-opacity="{op(.24 if dark else .17)}"/>
  <stop offset=".32" stop-color="{c['wave1']}" stop-opacity="{op(.51 if dark else .37)}"/>
  <stop offset=".63" stop-color="{c['ribbon']}" stop-opacity="{op(.31 if dark else .34)}"/>
  <stop offset="1" stop-color="{c['wave2']}" stop-opacity="{op(.53 if dark else .30)}"/>
 </linearGradient>
 <linearGradient id="silk" x1="0" y1="0" x2=".8" y2="1">
  <stop offset="0" stop-color="{c['highlight']}" stop-opacity="{op(.0)}"/>
  <stop offset=".15" stop-color="{c['highlight']}" stop-opacity="{op(.24 if dark else .37)}"/>
  <stop offset=".50" stop-color="{c['ribbon']}" stop-opacity="{op(.26 if dark else .45)}"/>
  <stop offset=".95" stop-color="{c['wave1']}" stop-opacity="{op(.05 if dark else .12)}"/>
 </linearGradient>
 <linearGradient id="foreground" x1="0" y1="0" x2="0.9" y2="1">
  <stop offset="0" stop-color="{c['under']}" stop-opacity="{op(.17 if dark else .10)}"/>
  <stop offset=".4" stop-color="{c['wave2']}" stop-opacity="{op(.33 if dark else .21)}"/>
  <stop offset="1" stop-color="{c['mid']}" stop-opacity="{op(.17 if dark else .13)}"/>
 </linearGradient>
 <linearGradient id="edge" x1="0" y1="0" x2="1" y2="0">
  <stop offset="0" stop-color="{c['edge']}" stop-opacity="0"/>
  <stop offset=".25" stop-color="{c['edge']}" stop-opacity="{op(.10 if dark else .35)}"/>
  <stop offset=".65" stop-color="{c['edge']}" stop-opacity="{op(.24 if dark else .45)}"/>
  <stop offset="1" stop-color="{c['edge']}" stop-opacity="0"/>
 </linearGradient>
</defs>
<rect width="960" height="544" fill="url(#base)"/>
<rect width="960" height="544" fill="url(#glow)"/>
<!-- Large translucent shells, strong silhouette without busy small details. -->
<path d="M-120 247 C92 274 171 370 363 375 C575 378 782 205 1080 140 L1080 340 C805 342 640 479 414 466 C169 451 81 343 -120 388 Z" fill="url(#far)" />
<path d="M-100 290 C107 334 239 430 425 395 C642 353 769 218 1080 216 L1080 320 C801 317 662 431 446 456 C234 490 61 368 -100 376 Z" fill="url(#main)" />
<path d="M-94 327 C108 368 229 470 403 438 C581 408 802 303 1040 291 L1040 343 C824 344 623 467 429 485 C217 505 36 397 -94 382 Z" fill="url(#silk)" />
<path d="M-128 396 C108 342 269 516 452 478 C658 435 773 360 1080 384 L1080 555 L-128 555 Z" fill="url(#foreground)" />
<path d="M-125 456 C66 425 230 538 410 502 C607 458 755 408 1090 434 L1090 560 L-125 560 Z" fill="url(#main)" opacity="{op(.60 if dark else .54)}" />
<!-- Quiet hairline contours, not bright full-screen strokes. -->
<path d="M-65 290 C130 322 238 424 425 393 C650 351 772 220 1038 217" fill="none" stroke="url(#edge)" stroke-width="2.3" />
<path d="M-95 327 C99 368 239 470 402 438 C602 399 816 300 1042 292" fill="none" stroke="url(#edge)" stroke-width="1.45" />
<path d="M-85 395 C112 354 272 514 455 478 C688 435 789 357 1060 385" fill="none" stroke="url(#edge)" stroke-width="1.25" opacity=".65" />
<!-- Subtle secondary upper-right wave adds Vita-shell depth without obscuring text. -->
<path d="M520 -18 C658 72 786 75 972 6 L972 101 C798 148 660 84 520 -18 Z" fill="{c['mist']}" opacity="{op(.085 if dark else .105)}"/>
<path d="M-55 92 C111 64 189 142 341 155 C460 167 537 153 620 137" fill="none" stroke="{c['ribbon']}" stroke-width="2" opacity="{op(.065 if dark else .09)}"/>
</svg>'''
  return svg

def generate(out: Path):
    paths=[]
    for theme in ('dark','light'):
        for alt in (False, True):
            label='background_alt_960x544.png' if alt else 'background_960x544.png'
            path=out/'themes'/theme/'fixed'/label
            path.parent.mkdir(parents=True, exist_ok=True)
            svg=svg_wallpaper(theme, alt)
            raw=cairosvg.svg2png(bytestring=svg.encode('utf-8'), output_width=1920,output_height=1088)
            img=Image.open(io.BytesIO(raw)).convert('RGBA').resize((W,H),Image.Resampling.LANCZOS)
            img.save(path, optimize=True)
            paths.append(path)
    return paths

if __name__=='__main__':
    out=Path(sys.argv[1]) if len(sys.argv)>1 else Path('/mnt/data/VitaGram_UI_Assets_v1')
    for path in generate(out):
        print(path, path.stat().st_size)
