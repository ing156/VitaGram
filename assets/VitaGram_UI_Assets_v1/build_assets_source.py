from __future__ import annotations
import io, json, math, os, shutil, zipfile, hashlib
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageFont
import cairosvg

OUT = Path('/mnt/data/VitaGram_UI_Assets_v1')
if OUT.exists(): shutil.rmtree(OUT)
OUT.mkdir(parents=True)
S=4
PALETTES={
 'dark': dict(background='#071A34', header_top='#0757B4', header_bottom='#073D83', surface='#112B4B', surface_secondary='#173759', hover='#1A4776', menu='#081C36', selected='#105CB4', accent='#149AFF', focus='#36BAFF', text='#F2F7FF', muted='#B1C8E5', divider='#294768', border='#30567B', shadow='#000B1C', date='#0E3C72', progress='#24496E', green='#23DB80'),
 'light':dict(background='#EDF5FD', header_top='#168EF1', header_bottom='#0871D3', surface='#FFFFFF', surface_secondary='#E3F0FC', hover='#DCEEFF', menu='#F5FAFF', selected='#C9E7FF', accent='#087EEA', focus='#088CFA', text='#102A50', muted='#57779E', divider='#CADCEC', border='#C1DAF1', shadow='#597FA5', date='#D6EBFC', progress='#C1D4E8', green='#12BB72')
}

def rgb(v):
 v=v.lstrip('#');return tuple(int(v[i:i+2],16) for i in (0,2,4))
def rgba(v,a=255):return rgb(v)+(a,)
def mix(a,b,t):return tuple(round(a[i]*(1-t)+b[i]*t) for i in range(3))
def font(sz,bold=False):
 p='/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf' if bold else '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
 return ImageFont.truetype(p,sz)
def save(img,p):
 p=OUT/p; p.parent.mkdir(parents=True,exist_ok=True); img.save(p,optimize=True)
 return str(p.relative_to(OUT))
manifest={'package':'VitaGram UI Assets','version':'1.2.0','screen':[960,544],'format':'PNG RGBA8','themes':PALETTES,'nine_slice':{},'textures':{},'sprite_atlases':{},'notes':['Source sizes are physical pixels for Vita frame buffer 960x544.','Nine-slice margin order is left,top,right,bottom; coordinates are top-left and width/height.','No logos, avatar images, user-specific text, or user-specific tabs are baked into textures.','Theme and per-component colors are static PNGs; real text stays in current app renderer.', 'Full-screen wave wallpapers are pre-rendered offline as opaque RGBA PNGs: background_960x544.png is the default and background_alt_960x544.png is an optional stronger version.']}

def make_nine(theme,name,fill,edge=None,size=(48,48),slice_=(12,12,12,12),radius=9,shadow=False,alpha=255,gradient=None,focus_stroke=0,ring_only=False):
 w,h=size
 # anti-aliased RGBA using high res
 W,H=w*S,h*S
 im=Image.new('RGBA',(W,H),(0,0,0,0))
 p=PALETTES[theme]
 gap=4 if shadow else 0
 box=(gap*S,gap*S,(w-gap)*S-1,(h-gap)*S-1)
 r=radius*S
 if shadow:
  sh=Image.new('RGBA',(W,H)); sm=Image.new('L',(W,H))
  draw=ImageDraw.Draw(sm)
  draw.rounded_rectangle((box[0],box[1]+3*S,box[2],box[3]+3*S),radius=r,fill=120 if theme=='light' else 80)
  sm=sm.filter(ImageFilter.GaussianBlur(3*S))
  sh.paste(rgba(p['shadow']), (0,0,W,H), sm)
  im=Image.alpha_composite(im,sh)
 if not ring_only:
  if gradient:
   gradient_im=Image.new('RGBA',(W,H))
   pix=gradient_im.load();a=rgb(gradient[0]);b=rgb(gradient[1])
   # static gentle y-gradient
   for y in range(H):
    t=y/max(H-1,1)
    row=mix(a,b,t)+(alpha,)
    for x in range(W):pix[x,y]=row
   mask=Image.new('L',(W,H))
   ImageDraw.Draw(mask).rounded_rectangle(box,radius=r,fill=255)
   gradient_im.putalpha(ImageChops.multiply(gradient_im.getchannel('A'),mask) if False else mask.point(lambda q: q*alpha//255))
   im=Image.alpha_composite(im,gradient_im)
  else:
   fg=Image.new('RGBA',(W,H),(0,0,0,0))
   ImageDraw.Draw(fg).rounded_rectangle(box,radius=r,fill=rgba(fill,alpha))
   im=Image.alpha_composite(im,fg)
 if edge:
  ImageDraw.Draw(im).rounded_rectangle((box[0]+S//2,box[1]+S//2,box[2]-S//2,box[3]-S//2),radius=max(r-S//2,1),outline=rgba(edge,230 if theme=='dark' else 210),width=S)
 if focus_stroke:
  ImageDraw.Draw(im).rounded_rectangle((box[0]+S,box[1]+S,box[2]-S,box[3]-S),radius=max(r-S,1),outline=rgba(p['focus'],255),width=focus_stroke*S)
 if ring_only:
  # alpha zero in central area and crisp ring
  ImageDraw.Draw(im).rounded_rectangle((box[0]+S,box[1]+S,box[2]-S,box[3]-S),radius=max(r-S,1),outline=rgba(p['focus'],255),width=2*S)
 im=im.resize((w,h),Image.Resampling.LANCZOS)
 f=save(im,f'themes/{theme}/9slice/{name}.png')
 manifest['nine_slice'][f]={'size':list(size),'insets':list(slice_),'radius':radius,'theme':theme,'name':name,'transparent':True}
 return f

W, H = 960, 544

WAVE_PALETTES = {
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
  c=WAVE_PALETTES[theme]
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


for theme,p in PALETTES.items():
 defs=[
  ('panel','surface','border',(48,48),(12,12,12,12),10,False,None,0,False),
  ('row','surface',None,(48,48),(12,12,12,12),8,False,None,0,False),
  ('row_focused','selected','focus',(48,48),(12,12,12,12),8,False,None,1,False),
  ('tab_normal','surface','border',(48,40),(12,12,12,12),8,False,None,0,False),
  ('tab_active','selected','accent',(48,40),(12,12,12,12),8,False,None,0,False),
  ('menu','menu','border',(64,64),(20,20,20,20),12,True,None,0,False),
  ('menu_item_focus','selected','focus',(48,48),(12,12,12,12),7,False,None,1,False),
  ('date_chip','date',None,(44,28),(14,10,14,10),12,False,None,0,False),
  ('file_tile','surface_secondary','border',(48,48),(12,12,12,12),8,False,None,0,False),
  ('tooltip','surface_secondary','border',(48,48),(12,12,12,12),8,False,None,0,False),
  ('toast','menu','border',(56,56),(16,16,16,16),10,True,None,0,False),
  ('media_frame','surface','border',(48,48),(12,12,12,12),9,False,None,0,False),
  ('focus_ring','surface','focus',(48,48),(12,12,12,12),8,False,None,0,True),
  ('badge','accent',None,(40,28),(12,10,12,10),11,False,None,0,False),
  ('dialog','surface','border',(64,64),(20,20,20,20),12,True,None,0,False),
  ('input','surface_secondary','border',(48,48),(12,12,12,12),8,False,None,0,False),
  ('progress_track','progress',None,(28,14),(8,5,8,5),5,False,None,0,False),
  ('progress_fill','accent',None,(28,14),(8,5,8,5),5,False,None,0,False),
  ('scroll_thumb','accent',None,(16,28),(5,8,5,8),5,False,None,0,False),
 ]
 for name,base,edge,sz,sl,r,sh,g,focus,ring in defs:
  make_nine(theme,name,p[base],p[edge] if edge else None,sz,sl,r,sh,gradient=g,focus_stroke=focus,ring_only=ring)

 # Static 960x544 Vita-inspired wave backgrounds. PNGs are ready-to-render;
 # the SVG geometry is only used by this offline asset generator.
 for alternate in (False, True):
  wallpaper_name = 'background_alt_960x544' if alternate else 'background_960x544'
  svg_source = svg_wallpaper(theme, alternate)
  png_bytes = cairosvg.svg2png(bytestring=svg_source.encode('utf-8'),
                               output_width=1920, output_height=1088)
  wallpaper = Image.open(io.BytesIO(png_bytes)).convert('RGBA')
  wallpaper = wallpaper.resize((960,544), Image.Resampling.LANCZOS)
  fp = save(wallpaper, f'themes/{theme}/fixed/{wallpaper_name}.png')
  manifest['textures'][fp] = {'size':[960,544], 'theme':theme,
    'role':'static fullscreen PS Vita-inspired wave wallpaper'+(' (alternative, stronger contrast)' if alternate else ' (main, restrained)'),
    'render':'single texture blit; no runtime wave generation'}
 for name,height,ca,cb in [('header_960x40',40,p['header_top'],p['header_bottom']),('footer_960x44',44,p['header_bottom'],p['header_top'])]:
  tile=Image.new('RGBA',(960,height)); d=ImageDraw.Draw(tile)
  aa=rgb(ca);bb=rgb(cb)
  for y in range(height):
   t=y/max(1,height-1)
   d.line((0,y,960,y),fill=mix(aa,bb,t)+(255,))
  # top highlight & hairline bottom
  d.line((0,0,959,0),fill=(*mix(aa,(255,255,255),.15),160),width=1)
  d.line((0,height-1,959,height-1),fill=rgba(p['border'],225),width=1)
  fp=save(tile,f'themes/{theme}/fixed/{name}.png')
  manifest['textures'][fp]={'size':[960,height],'theme':theme,'role':name}
 # accent indicator for tab when active (actual width scaled at runtime)
 tile=Image.new('RGBA',(128,4),(0,0,0,0));d=ImageDraw.Draw(tile)
 d.rounded_rectangle((0,0,127,3),radius=2,fill=rgba(p['accent']))
 fp=save(tile,f'themes/{theme}/fixed/active_indicator_128x4.png')
 manifest['textures'][fp]={'size':[128,4],'theme':theme,'role':'optional active tab underline'}

# Icon stroke glyphs as self-authored scalable SVG shapes (24x24 viewbox)
PATH={
 'play':'<path d="M8 5.5 19 12 8 18.5Z"/>',
 'pause':'<path d="M8 5V19M16 5V19"/>',
 'back':'<path d="m14.5 5-7 7 7 7M8 12h12"/>',
 'next':'<path d="m9.5 5 7 7-7 7"/>',
 'refresh':'<path d="M20 6v5h-5M4 18v-5h5"/><path d="M5.8 9.2A7 7 0 0 1 18 6l2 5M4 13l2 5a7 7 0 0 0 12-3.2"/>',
 'attach':'<path d="m9 13.4 6.7-6.7a3.3 3.3 0 1 1 4.7 4.7l-9.1 9a5 5 0 0 1-7-7l9.4-9.4"/>',
 'file':'<path d="M6 3h8l5 5v13H6Z"/><path d="M14 3v6h5M9 14h6M9 17h6"/>',
 'image':'<rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="8.5" cy="9" r="1.5"/><path d="m4 18 5-5 4 3 3-4 5 6"/>',
 'video':'<rect x="3" y="5" width="18" height="14" rx="2"/><path d="m3 9 4 4 4-4 4 4 4-4"/><path d="M8 5v14" opacity=".3"/>',
 'download':'<path d="M12 3v12m-5-5 5 5 5-5M4 17v3h16v-3"/>',
 'send':'<path d="m3 11 18-8-7 18-3.5-7-7.5-3ZM10.5 14 21 3"/>',
 'search':'<circle cx="10.5" cy="10.5" r="6.5"/><path d="m16 16 5 5"/>',
 'folder':'<path d="M3 6h7l2 3h9v11H3Z"/>',
 'chat':'<path d="M20.5 12a8.5 8.5 0 0 1-8.5 8.5c-1.8 0-3.4-.4-4.7-1.4L3 20.5l1.4-4.2A8.5 8.5 0 1 1 20.5 12Z"/>',
 'mic':'<rect x="9" y="3" width="6" height="12" rx="3"/><path d="M6 11a6 6 0 0 0 12 0M12 17v4m-4 0h8"/>',
 'heart':'<path d="M20 8c0 5-8 11-8 11S4 13 4 8a4.2 4.2 0 0 1 8-1 4.2 4.2 0 0 1 8 1Z"/>',
 'forward':'<path d="M14 4 21 11l-7 7v-4C8 14 5 15 3 20c0-8 4-12 11-12Z"/>',
 'fullscreen':'<path d="M9 3H3v6M15 3h6v6M3 15v6h6M21 15v6h-6"/>',
 'collapse':'<path d="M8 3v5H3M16 3v5h5M3 16h5v5M21 16h-5v5"/>',
 'more':'<circle cx="5" cy="12" r="1"/><circle cx="12" cy="12" r="1"/><circle cx="19" cy="12" r="1"/>',
 'sticker':'<path d="M4 3h16v11l-7 7H4Z"/><path d="M13 21v-7h7M8 9h.01M15 9h.01M8 13c2 2 5 2 7 0"/>',
 'warning':'<path d="M12 3 22 21H2Z"/><path d="M12 9v5m0 3v.5"/>',
 'info':'<circle cx="12" cy="12" r="9"/><path d="M12 11v6m0-10v.5"/>',
 'list':'<path d="M9 6h12M9 12h12M9 18h12M4 6h.5M4 12h.5M4 18h.5"/>',
 'edit':'<path d="m4 20 5-1 11-11-4-4L5 15Z"/><path d="M14 6l4 4"/>',
 'check':'<path d="m4.5 12 5.5 5.5L20 6"/>',
 'menu':'<path d="M4 6h16M4 12h16M4 18h16"/>',
 'close':'<path d="M5 5 19 19M19 5 5 19"/>',
 'volume':'<path d="M4 9h4l5-5v16l-5-5H4ZM17 8a6 6 0 0 1 0 8M19.5 5a10 10 0 0 1 0 14"/>',
 'calendar':'<rect x="3" y="5" width="18" height="16" rx="2"/><path d="M7 3v5M17 3v5M3 10h18M8 14h3M8 17h3"/>',
 'wifi':'<path d="M2 8a15 15 0 0 1 20 0M5 12a10 10 0 0 1 14 0M9 16a4 4 0 0 1 6 0"/><circle cx="12" cy="20" r=".9" fill="currentColor" stroke="none"/>',
 'settings':'<circle cx="12" cy="12" r="3"/><path d="M10 2h4l.7 3 2 1.2 2.7-.9 2 3.4-2 2.1v2.4l2 2.1-2 3.4-2.7-.9-2 1.2L14 22h-4l-.7-3-2-1.2-2.7.9-2-3.4 2-2.1v-2.4l-2-2.1 2-3.4 2.7.9 2-1.2Z"/>',
 'arrow-up':'<path d="m5 14 7-7 7 7M12 7v13"/>',
 'arrow-down':'<path d="m5 10 7 7 7-7M12 17V4"/>',
}

def icon_png(name, hex_color):
 body=PATH[name]
 svg=f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="128" height="128"><g fill="none" stroke="{hex_color}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">{body}</g></svg>'
 raw=cairosvg.svg2png(bytestring=svg.encode(),output_width=128,output_height=128)
 return Image.open(io.BytesIO(raw)).convert('RGBA').resize((32,32),Image.Resampling.LANCZOS)

BATTERY_STATES=['battery_0','battery_25','battery_50','battery_75','battery_100','battery_charging']

def battery_icon_png(name,theme):
    level=int(name.split('_')[1]) if name!='battery_charging' else 75
    outline='#F2F7FF' if theme=='dark' else '#173D65'
    color=('#F36B85' if level==0 else '#F5B357' if level==25 else '#36BAFF' if theme=='dark' else '#087EEA')
    if name=='battery_charging': color='#30E08A' if theme=='dark' else '#00B87A'
    inner_w=12.2; w=inner_w*level/100
    fill=(f'<rect x="5" y="8.75" width="{w:.2f}" height="6.5" rx="1.1" fill="{color}" />' if level else '<rect x="5" y="8.75" width="1.55" height="6.5" rx=".7" fill="#F36B85" />')
    bolt=''
    if name=='battery_charging':
        fill='<rect x="5" y="8.75" width="12.3" height="6.5" rx="1.1" fill="#30E08A" opacity="0.25" />'
        bolt='<path d="M12.8 5.9 9.6 12.2h3.05l-1.3 5.6 5.1-7h-3.2l1.4-4.9Z" fill="'+color+'" stroke="'+outline+'" stroke-width=".45" stroke-linejoin="round"/>'
    svg=f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="128" height="128">
    <rect x="3.3" y="6.45" width="16" height="11.15" rx="2.2" stroke="{outline}" stroke-width="1.75" fill="none"/>
    <path d="M19.2 10h1.1a1.05 1.05 0 0 1 1.05 1.05v1.9a1.05 1.05 0 0 1-1.05 1.05h-1.1" stroke="{outline}" stroke-width="1.65" stroke-linecap="round" fill="none"/>
    {fill}{bolt}</svg>'''
    return Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svg.encode(),output_width=128,output_height=128))).convert('RGBA').resize((32,32),Image.Resampling.LANCZOS)

manifest['battery_icons']={'states':BATTERY_STATES,'source_size':[32,32],'recommended_size':[20,20],
    'display_rules':{'charging':'battery_charging','not_charging':{'0-12':'battery_0','13-37':'battery_25','38-62':'battery_50','63-87':'battery_75','88-100':'battery_100'}},
    'note':'Visual only: real charge values must come from the application/system.'}

for theme,p in PALETTES.items():
 names=list(PATH)+BATTERY_STATES
 cols=8;cell=64; rows=math.ceil(len(names)/cols)
 atlas=Image.new('RGBA',(cols*cell,rows*cell))
 sprites={}
 for i,name in enumerate(names):
  icon=icon_png(name,p['text']) if name in PATH else battery_icon_png(name,theme)
  save(icon,f'themes/{theme}/icons/{name}.png')
  x=(i%cols)*cell+16;y=(i//cols)*cell+16
  atlas.alpha_composite(icon,(x,y))
  sprites[name]={'x':x,'y':y,'w':32,'h':32}
 fp=save(atlas,f'themes/{theme}/ui_icons_atlas.png')
 manifest['sprite_atlases'][fp]={'size':list(atlas.size),'cell':[64,64],'sprites':sprites,'theme':theme,'note':'32x32 glyphs; x/y refer to glyph bounds, not full cell'}

BUTTONS=['cross','circle','square','triangle','dpad','L','R','START','SELECT']
BTN_COLORS={'cross':'#26B2FF','circle':'#FA6078','square':'#F27CCC','triangle':'#39D990'}

def make_button(theme,name):
 # 48x48, supersampled, antialiased clear background
 p=PALETTES[theme];w=h=48
 img=Image.new('RGBA',(w*S,h*S),(0,0,0,0));d=ImageDraw.Draw(img)
 col=BTN_COLORS.get(name, '#C9DCF2' if theme=='dark' else '#3D6187')
 ink=rgba(col)
 k=lambda v:round(v*S)
 if name=='cross':
  d.line([(k(14),k(14)),(k(34),k(34))],fill=ink,width=k(4),joint='curve')
  d.line([(k(34),k(14)),(k(14),k(34))],fill=ink,width=k(4),joint='curve')
 elif name=='circle':d.ellipse((k(13),k(13),k(35),k(35)),outline=ink,width=k(4))
 elif name=='square':d.rounded_rectangle((k(13),k(13),k(35),k(35)),radius=k(1.5),outline=ink,width=k(4))
 elif name=='triangle':d.line([(k(24),k(11)),(k(36),k(34)),(k(12),k(34)),(k(24),k(11))],fill=ink,width=k(4),joint='curve')
 elif name=='dpad':
  pts=[(18,8),(30,8),(30,18),(40,18),(40,30),(30,30),(30,40),(18,40),(18,30),(8,30),(8,18),(18,18)]
  d.polygon([(k(x),k(y)) for x,y in pts],fill=rgba(p['text'],215))
  d.ellipse((k(21),k(21),k(27),k(27)),fill=rgba(p['background']))
 else:
  d.rounded_rectangle((k(2),k(14),k(46),k(34)),radius=k(8),fill=rgba(p['surface_secondary'],255),outline=rgba(p['border']),width=k(1))
  t=name
  f=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 10*S if len(t)>2 else 15*S)
  bbox=d.textbbox((0,0),t,font=f)
  d.text(((48*S-(bbox[2]-bbox[0]))/2, (48*S-(bbox[3]-bbox[1]))/2-bbox[1]),t,font=f,fill=rgba(p['text']))
 return img.resize((w,h),Image.Resampling.LANCZOS)
for theme in PALETTES:
 atlas=Image.new('RGBA',(512,128)); sprites={}
 for i,name in enumerate(BUTTONS):
  icon=make_button(theme,name)
  save(icon,f'themes/{theme}/buttons/{name}.png')
  x=i%8*64+8;y=i//8*64+8
  atlas.alpha_composite(icon,(x,y))
  sprites[name]={'x':x,'y':y,'w':48,'h':48}
 fp=save(atlas,f'themes/{theme}/ps_buttons_atlas.png')
 manifest['sprite_atlases'][fp]={'size':[512,128],'cell':[64,64],'sprites':sprites,'theme':theme,'note':'48x48 RGBA glyphs; button mapping is presentation only, not input handling'}

# Reusable small decorative overlays
for theme,p in PALETTES.items():
 # 120x12 subtle line glow, can stretch along focused row as decorative accent
 w,h=16,128
 im=Image.new('RGBA',(w,h)); px=im.load();c=rgb(p['accent'])
 for y in range(h):
  for x in range(w):
   strength=(1-x/16)**2 *(.4+.6*max(0,1-abs(y-64)/74))
   px[x,y]=c+(round(150*strength),)
 fp=save(im,f'themes/{theme}/fixed/focus_edge_glow_16x128.png')
 manifest['textures'][fp]={'size':[16,128],'theme':theme,'role':'optional overlay for row focus'}

# manifest in source root
(OUT/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')

# Pillow nine-slice compositor used only to verify actual assets.
def nine(dst,asset,box):
 f=OUT/asset; src=Image.open(f).convert('RGBA'); meta=manifest['nine_slice'][asset]
 l,t,r,b=meta['insets'];sw,sh=src.size;x,y,w,h=map(int,box)
 if w<l+r or h<t+b: raise ValueError((asset,box,meta['insets']))
 cols=[(0,l,x,l),(l,sw-l-r,x+l,w-l-r),(sw-r,r,x+w-r,r)]
 rows=[(0,t,y,t),(t,sh-t-b,y+t,h-t-b),(sh-b,b,y+h-b,b)]
 for sy,ssh,dy,dh in rows:
  for sx,ssw,dx,dw in cols:
   if dw<=0 or dh<=0:continue
   sub=src.crop((sx,sy,sx+ssw,sy+ssh))
   if sub.size!=(dw,dh): sub=sub.resize((dw,dh),Image.Resampling.BILINEAR)
   dst.alpha_composite(sub,(dx,dy))

def text_at(img,pos,string,sz,color,bold=False):
 ImageDraw.Draw(img).text(pos,string,font=font(sz,bold),fill=rgba(color),stroke_width=0)

def label_cut(s,limit=37):return s[:limit-1]+'…' if len(s)>limit else s

def sprite(img,theme,atlas,icon,pos,w=None):
 meta=manifest['sprite_atlases'][f'themes/{theme}/{atlas}.png'];r=meta['sprites'][icon]
 src=Image.open(OUT/f'themes/{theme}/{atlas}.png').crop((r['x'],r['y'],r['x']+r['w'],r['y']+r['h']))
 if w:src=src.resize((w,w),Image.Resampling.LANCZOS)
 img.alpha_composite(src,pos)

def make_demo(theme,mode):
 p=PALETTES[theme]
 img=Image.open(OUT/f'themes/{theme}/fixed/background_960x544.png').convert('RGBA')
 top=Image.open(OUT/f'themes/{theme}/fixed/header_960x40.png')
 img.alpha_composite(top,(0,0))
 sprite(img,theme,'ui_icons_atlas','send',(14,4),30)
 text_at(img,(53,7),'VitaGram',21,'#FFFFFF',True)
 text_at(img,(178,14),'0.36.17  •  UI concept',11,'#CFEAFF')
 d=ImageDraw.Draw(img)
 d.ellipse((825,17,834,26),fill=rgba(p['green']))
 text_at(img,(840,12),'В сети',14,'#FFFFFF')
 if mode=='list':
  # set actual dynamic example tabs different in themes
  tabnames=['Все','Новости','Избранное','Работа','Другое'] if theme=='dark' else ['Все','Проекты']
  ty=44
  x=18
  for i,name in enumerate(tabnames):
   w=min(230,max(84,int(d.textbbox((0,0),name,font=font(16))[2]+38)))
   nine(img,f'themes/{theme}/9slice/'+('tab_active' if i==0 else 'tab_normal')+'.png',(x,ty,w,36))
   text_at(img,(x+16,ty+8),name,15, p['text'],bold=(i==0))
   x+=w+7
  names=[('Заметки и идеи','Работа над дизайном'),('Новости технологий','Новые публикации'),('Александр','Отправил фотографию'),('Ретро-игры','Обсуждение консолей'),('Городские события','Обновление канала'),('Музыка и подкасты','Сегодня в 13:20'),('Чат разработки','Правки интерфейса'),('Архив','Сохранённые сообщения')]
  for i,(name,sub) in enumerate(names):
   y=87+i*48
   nine(img,f'themes/{theme}/9slice/'+('row_focused' if i==2 else 'row')+'.png',(18,y,924,46))
   # avatar squares are demo placeholders, runtime user images are separate.
   avcol=mix(rgb(p['header_top']),rgb(p['surface_secondary']),.35+i*.035)
   d.rounded_rectangle((31,y+5,67,y+41),radius=9,fill=avcol+(255,))
   avatar_icons=['edit','info','chat','play','calendar','volume','settings','folder']
   sprite(img,theme,'ui_icons_atlas',avatar_icons[i],(37,y+11),24)
   text_at(img,(83,y+4),name,16,p['text'],True)
   text_at(img,(83,y+25),sub,12,p['muted'])
  statusy=476
  d.rectangle((0,statusy,959,statusy+23),fill=rgba(p['surface_secondary']))
  text_at(img,(20,statusy+5),'Загружено 8 чатов · демонстрационные данные',12,p['muted'])
  img.alpha_composite(Image.open(OUT/f'themes/{theme}/fixed/footer_960x44.png'),(0,500))
  for idx,(sym,label) in enumerate([('dpad','Выбор'),('cross','Открыть'),('triangle','Обновить'),('START','Настройки')]):
   x=[22,250,454,690][idx]
   sprite(img,theme,'ps_buttons_atlas',sym,(x,499),40)
   text_at(img,(x+41,514),label,14,'#FFFFFF')
 elif mode=='chat':
  text_at(img,(36,53),'История сообщений',15,p['text'],True)
  # dates
  nine(img,f'themes/{theme}/9slice/date_chip.png',(390,85,180,24))
  text_at(img,(410,89),'9 октября 2026',13,p['muted'])
  nine(img,f'themes/{theme}/9slice/row.png',(40,122,574,70))
  text_at(img,(54,132),'Добро пожаловать в чат!',17,p['text'],True)
  text_at(img,(54,162),'Напишите сообщение или откройте вложение',13,p['muted'])
  nine(img,f'themes/{theme}/9slice/row_focused.png',(40,203,574,114))
  sprite(img,theme,'ui_icons_atlas','file',(53,225),36)
  text_at(img,(101,215),'UI_assets_preview.png',17,p['text'],True)
  text_at(img,(101,242),'Предпросмотр вложения',14,p['muted'])
  text_at(img,(101,276),'Файл',13,p['muted'])
  text_at(img,(545,276),'15:42',13,p['muted'])
  nine(img,f'themes/{theme}/9slice/row.png',(40,328,574,84))
  text_at(img,(54,342),'Текст сообщения с выделением',17,p['text'],True)
  text_at(img,(54,372),'09:34',13,p['muted'])
  # context menu on right
  menu_x,menu_y=602,100
  nine(img,f'themes/{theme}/9slice/menu.png',(menu_x,menu_y,332,332))
  entries=[('cross','Открыть пост'),('square','Написать сообщение'),('L','Видеокружок'),('R','Голосовое'),('SELECT','Прикрепить файл'),('triangle','Обновить'),('circle','Назад')]
  for i,(btn,title) in enumerate(entries):
   yy=110+44*i
   if i==0: nine(img,f'themes/{theme}/9slice/menu_item_focus.png',(610,yy,316,42))
   sprite(img,theme,'ps_buttons_atlas',btn,(616,yy+1),40)
   text_at(img,(659,yy+9),title,15,p['text'])
  img.alpha_composite(Image.open(OUT/f'themes/{theme}/fixed/footer_960x44.png'),(0,500))
  sprite(img,theme,'ps_buttons_atlas','circle',(25,500),40)
  text_at(img,(74,513),'Назад',14,'#FFFFFF')
 return img

for theme in PALETTES:
 for mode in ('list','chat'):
  demo=make_demo(theme,mode)
  save(demo,f'previews/{theme}_{mode}_960x544.png')

# side-by-side sampler / visual proof
p1=Image.open(OUT/'previews/dark_list_960x544.png');p2=Image.open(OUT/'previews/light_list_960x544.png')
out=Image.new('RGB',(1920,544));out.paste(p1,(0,0),p1);out.paste(p2,(960,0),p2)
out.save('/mnt/data/vitagram_assets_light_dark_preview.png',optimize=True)

# Asset sheet, using large cards and labels
sheet=Image.new('RGBA',(1560,960),rgba('#0A1628'))
d=ImageDraw.Draw(sheet)
text_at(sheet,(35,20),'VitaGram  •  UI assets v1.1',30,'#FFFFFF',True)
text_at(sheet,(36,63),'PNG RGBA  ·  9-slice panels  ·  PS buttons  ·  icon atlases  ·  two themes',17,'#A4C6E7')
for col,theme in enumerate(('dark','light')):
 p=PALETTES[theme];x0=30+col*770;y0=110
 d.rounded_rectangle((x0,y0,x0+738,y0+728),radius=22,fill=rgb(p['background']))
 header=Image.open(OUT/f'themes/{theme}/fixed/header_960x40.png').resize((712,40),Image.Resampling.BILINEAR)
 sheet.paste(header,(x0+12,y0+16),header)
 text_at(sheet,(x0+29,y0+22),theme.upper()+' THEME',18,'#FFFFFF',True)
 pieces=[('panel','Panel'),('row','Chat row'),('row_focused','Focused row'),('tab_normal','Tab'),('tab_active','Active tab'),('menu','Menu panel'),('menu_item_focus','Menu focus'),('date_chip','Date chip'),('file_tile','File tile'),('toast','Toast'),('media_frame','Media frame'),('focus_ring','Focus border'),('badge','Badge'),('progress_track','Progress track')]
 for i,(nm,label) in enumerate(pieces):
  rr=i//2;cc=i%2
  xx=x0+22+cc*355; yy=y0+77+rr*76
  w=312 if nm not in ('badge','date_chip','progress_track') else 230
  h=43 if nm not in ('badge','date_chip','progress_track') else 29
  nine(sheet,f'themes/{theme}/9slice/{nm}.png',(xx,yy+21,w,h))
  text_at(sheet,(xx+3,yy),label,12,p['muted'])
  if nm not in ('focus_ring','progress_track'):
   text_at(sheet,(xx+14,yy+30),'VitaGram UI',13,p['text'],True)
 sprite_y=y0+623
 text_at(sheet,(x0+26,sprite_y),'Controller buttons',12,p['muted'])
 for i,name in enumerate(BUTTONS[:7]):
  sprite(sheet,theme,'ps_buttons_atlas',name,(x0+25+i*72,sprite_y+22),44)
 text_at(sheet,(x0+26,y0+694),'Icon sheet: 32px PNGs, also available individually',12,p['muted'])
 for i,name in enumerate(['play','pause','refresh','attach','file','settings','chat','search']):
  sprite(sheet,theme,'ui_icons_atlas',name,(x0+27+i*72,y0+713),30)
sheet.save('/mnt/data/vitagram_assets_catalog.png',optimize=True)

# Examples include verified Vita2D header method signatures (libvita2d / xerpi).
example = r'''// VitaGram UI PNG assets: minimal draw helpers for an existing vita2d renderer.
// Source reference: libvita2d/include/vita2d.h (check your own fork).
// Example intentionally does NOT change your input or screen architecture.
#include <vita2d.h>

struct NineSlice { int left, top, right, bottom; };

static void draw_9slice(const vita2d_texture *tex,
                        float x, float y, float w, float h,
                        NineSlice m) {
    if (!tex || w < m.left+m.right || h < m.top+m.bottom) return;
    const float tw = (float)vita2d_texture_get_width(tex);
    const float th = (float)vita2d_texture_get_height(tex);
    const float sw[3] = { (float)m.left, tw-m.left-m.right, (float)m.right };
    const float sh[3] = { (float)m.top, th-m.top-m.bottom, (float)m.bottom };
    const float dw[3] = { (float)m.left, w-m.left-m.right, (float)m.right };
    const float dh[3] = { (float)m.top, h-m.top-m.bottom, (float)m.bottom };
    float sy=0, dy=y;
    for (int row=0; row<3; ++row) {
        float sx=0, dx=x;
        for (int col=0; col<3; ++col) {
            if (sw[col] > 0 && sh[row] > 0 && dw[col] > 0 && dh[row] > 0)
                vita2d_draw_texture_part_scale(tex, dx, dy, sx, sy,
                    sw[col], sh[row], dw[col]/sw[col], dh[row]/sh[row]);
            sx+=sw[col]; dx+=dw[col];
        }
        sy+=sh[row]; dy+=dh[row];
    }
}

// Load ONCE during initialization; paths reflect your installed resource folder:
// vita2d_texture *row = vita2d_load_PNG_file("app0:assets/dark/row.png");
// Draw as an ordinary rectangle panel with rounded corners from the image:
// draw_9slice(row, 18, 82, 924, 48, {12,12,12,12});
// Do not load PNGs in the frame loop. Free with vita2d_free_texture() at exit.
//
// For atlas icons, manifest.json gives the precise source x/y/width/height.
// Example 32px icon from a themed atlas (no resizing):
// vita2d_draw_texture_part(ui_icons, dst_x, dst_y, src_x, src_y, 32, 32);
// For a 48px Vita button from buttons atlas:
// vita2d_draw_texture_part(buttons, dst_x, dst_y, src_x, src_y, 48, 48);
// PNGs use straight RGBA8 and transparent pixels. Check blending enabled by your build.
'''
(OUT/'example_vita2d.cpp').write_text(example,encoding='utf-8')

readme='''# VitaGram UI Assets v1.2

Готовый набор PNG-текстур для оформления **существующего** UI VitaGram в vita2d.\nЭто визуальный ресурсный пакет, а не переписывание текущей логики и не готовое приложение.\n\n## Состав\n\n- `themes/dark/9slice/` и `themes/light/9slice/` — панели, строки, вкладки, меню, фокус, скроллбар, прогресс, бейджи и т. д. с RGBA-прозрачностью.\n- `themes/*/fixed/` — волнообразные фоны 960×544 (основной и альтернативный), градиентная шапка 960×40 и нижняя панель 960×44, индикаторы.\n- `themes/*/icons/` — отдельные контурные иконки 32×32.\n- `themes/*/buttons/` — отдельные кнопки ×, ○, □, △, L, R, START, SELECT, крестовина; 48×48.\n- `themes/*/ui_icons_atlas.png` — объединение иконок.\n- `themes/*/ps_buttons_atlas.png` — объединение обозначений кнопок.\n- `manifest.json` — имена, размеры, отступы 9-slice и координаты спрайтов в атласах.\n- `previews/` — демонстрационные экраны 960×544, НЕ реальные скриншоты VitaGram.\n- `example_vita2d.cpp` — минимальная вспомогательная функция 9-slice.\n\n## Использование с vita2d\n\n1. Скопируйте нужные PNG в `app0:...` и загрузите их с `vita2d_load_PNG_file()` при инициализации экрана/темы.\n2. Вызовите `draw_9slice()` с параметрами из `manifest.json`. Это девять текстурных блитов — **не рисование геометрии**.\n3. Иконки рендерьте через `vita2d_draw_texture_part()` из общего атласа либо загружайте индивидуальные PNG.\n4. Для переключения темы загрузите соответствующий набор ресурсов и корректно выгрузите старые текстуры после завершения их использования.\n5. Текст, аватары, превью, пользовательские вкладки, данные меню и видеокадры оставьте динамическими в текущем коде.\n\n## Важные ограничения\n\n- Все файлы RGBA PNG; текстуры с прозрачностью.\n- `9-slice` не означает единственный draw call: один элемент обычно требует 9 фрагментов текстуры.\n- Не сжимайте угол 9-slice: итоговый размер элемента >= сумме соответствующих отступов.\n- Никаких фотографий, пользовательских текстов или количества вкладок в PNG не зашито.\n- Атлас иконок имеет прозрачный фон, координаты берите из манифеста.\n- Ресурсы полностью созданы для этого пакета; готовые системные файлы/шрифты PS Vita не включены.\n- Надписи и текст в previews — демонстрационные; шрифты из окружения сборки НЕ включаются в пакет.\n- Сборка и запуск на реальной PS Vita не выполнялись. Совместимость с используемым форком vita2d надо проверить.\n\n## Рекомендации по оптимизации\n\n- Загружайте текстуры один раз.\n- Используйте `ui_icons_atlas.png` и `ps_buttons_atlas.png` вместо большого числа маленьких отдельных загрузок.\n- Избегайте загрузки обеих тем одновременно при дефиците памяти.\n- Фиксированные фоны и шапки можно рисовать одним блитом.\n- Не создавайте новые текстуры на каждом кадре.\n- Старайтесь сгруппировать отрисовку 9-slice панелей по текстуре/экрану.\n'''
(OUT/'README_RU.md').write_text(readme+'\n## Battery assets (v1.2)\nSix 32x32 state icons per theme: battery_0, battery_25, battery_50, battery_75, battery_100, battery_charging. They are packed in the remaining unused cells of ui_icons_atlas.png; original coordinates stay unchanged. Use live battery data.\n',encoding='utf-8')

# Ship the exact reproducible asset generator and updated mapping with the pack.
(OUT/'build_assets_source.py').write_text(Path(__file__).read_text(encoding='utf-8'), encoding='utf-8')
(OUT/'generate_wave_backgrounds.py').write_text(Path('/mnt/data/generate_wave_backgrounds.py').read_text(encoding='utf-8'), encoding='utf-8')
if Path('/mnt/data/VitaGram_mapping_orig.md').exists():
 (OUT/'MAPPING_RU.md').write_text(Path('/mnt/data/VitaGram_mapping_orig.md').read_text(encoding='utf-8'), encoding='utf-8')

# inspect pack integrity
issues=[]
for path,dat in manifest['nine_slice'].items():
 im=Image.open(OUT/path)
 if im.mode!='RGBA':issues.append('mode '+path)
 W,H=im.size; l,t,r,b=dat['insets'];
 if l+r>=W or t+b>=H: issues.append('nine bounds '+path)
 if im.getextrema()[3][0] == 255:issues.append('no alpha '+path)
for atlas,dat in manifest['sprite_atlases'].items():
 im=Image.open(OUT/atlas)
 for nm,rect in dat['sprites'].items():
  if rect['x']+rect['w']>im.width or rect['y']+rect['h']>im.height: issues.append('atlas bound '+atlas+' '+nm)
assert not issues,issues
files=[p for p in OUT.rglob('*') if p.is_file()]
zipf=Path('/mnt/data/VitaGram_vita2d_UI_Assets_v1_2.zip')
with zipfile.ZipFile(zipf,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
 for p in files:z.write(p,arcname='VitaGram_UI_Assets_v1/'+str(p.relative_to(OUT)))
print(f'PASS: 9-slice sprites={len(manifest["nine_slice"])}')
print(f'PASS: atlases={len(manifest["sprite_atlases"])}; icons/theme={len(PATH)+len(BATTERY_STATES)}; PS buttons/theme={len(BUTTONS)}')
print(f'PASS: texture files={len(list(OUT.rglob("*.png")))}; total files={len(files)}')
print(f'PASS: sample screen previews=4; atlas bounds and alpha slices valid')
print('ZIP:',zipf, zipf.stat().st_size,'bytes')
print('CATALOG: /mnt/data/vitagram_assets_catalog.png')
print('DUAL: /mnt/data/vitagram_assets_light_dark_preview.png')
