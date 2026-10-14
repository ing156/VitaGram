"""Adapt VHBB surfaces to VitaGram's manifest/9-slice format (offline, Pillow).

Run with external/font-tools-venv/bin/python from the repository root.
Only the vhbb theme and its manifest entries are written; existing themes are preserved.
The original source PNGs are included with their CC BY-NC-SA 4.0 notice.
"""
from pathlib import Path
import copy
import json
import math
import shutil
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
BASE = ROOT / 'themes/vhbb'
SOURCE = BASE / 'source'
manifest = json.loads((ROOT / 'manifest.json').read_text())
palette = dict(manifest['themes']['dark'], background='#12246B', surface='#343C79',
               header_top='#427ECA', header_bottom='#183373', selected='#117BDD',
               text='#F2F7FF', muted='#C2CFED', border='#829DCE', menu='#25282D')
manifest['themes']['vhbb'] = palette


def save(image, relative, section, template=None):
    path = BASE / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    image.convert('RGBA').save(path, optimize=True)
    key = 'themes/vhbb/' + relative
    meta = copy.deepcopy(template or {})
    meta.update(size=list(image.size), theme='vhbb')
    manifest[section][key] = meta


def rounded_gradient(source, size, focused=False, flat=None, ring=False):
    """Sample VHBB's blue/violet gradient, leaving white label bands out of text areas."""
    w, h = size
    scale = 4
    image = Image.new('RGBA', (w*scale, h*scale))
    draw = ImageDraw.Draw(image)
    for y in range(h*scale):
        sample_y = round((source.height-1) * y / max(1, h*scale-1))
        color = source.getpixel((source.width//2, sample_y))[:3]
        if flat:
            color = flat
        # Darken focus surfaces for readable white labels, including over transparent cards.
        if focused:
            color = tuple(round(c*0.76) for c in color)
        draw.line((0,y,w*scale,y), fill=(*color,255))
    mask = Image.new('L', image.size)
    ImageDraw.Draw(mask).rounded_rectangle((0,0,w*scale-1,h*scale-1), radius=7*scale, fill=255)
    image.putalpha(mask)
    draw.rounded_rectangle((2,2,w*scale-3,h*scale-3), radius=7*scale,
                           outline=(139,193,255,255) if focused else (123,141,194,255), width=scale)
    draw.line((8*scale,scale,(w-8)*scale,scale), fill=(214,232,255,255), width=scale)
    if ring:
        ImageDraw.Draw(image).rounded_rectangle((3*scale,3*scale,(w-3)*scale-1,(h-3)*scale-1),
                                               radius=4*scale, fill=(0,0,0,0))
    return image.resize(size, Image.Resampling.LANCZOS)


normal = Image.open(SOURCE/'img_itm_panel.png').convert('RGBA')
normal = normal.crop((0,36,normal.width,normal.height))
focus = Image.open(SOURCE/'img_itm_panel_highlight.png').convert('RGBA')
dialog = Image.open(SOURCE/'img_dialog_msg_bg.png').convert('RGBA')
focus_names = {'row_focused','tab_active','menu_item_focus','progress_fill','scroll_thumb'}
for key, meta in list(manifest['nine_slice'].items()):
    if not key.startswith('themes/dark/'):
        continue
    name = meta['name']
    flat = (37,40,45) if name in {'dialog','menu'} else (52,60,121) if name == 'media_frame' else None
    src = dialog if name in {'dialog','menu'} else focus if name in focus_names else normal
    im = rounded_gradient(src, tuple(meta['size']), name in focus_names, flat, name=='focus_ring')
    save(im, '9slice/'+name+'.png', 'nine_slice', meta)

# A fresh wallpaper with VHBB's blue glow and translucent squares, without its branding.
w, h = 960, 544
bg = Image.new('RGBA', (w,h))
pixels = bg.load()
for y in range(h):
    for x in range(w):
        glow = math.exp(-((x-320)**2/240000 + (y-255)**2/95000))
        pixels[x,y] = (round(16+3*glow), round(28+53*glow), round(88+88*glow),255)
overlay = Image.new('RGBA',bg.size)
d = ImageDraw.Draw(overlay)
for i in range(55):
    x, y, size = (i*173+29)%960, (i*97+13)%544, 12+(i*19)%76
    d.rectangle((x,y,x+size,y+size), fill=(87,158,239,10+i%4*4))
for i in range(6):
    points = [(round(695+80*math.sin(y/245+i*0.17)+i*22), y) for y in range(-20,565,4)]
    d.line(points, fill=(144,205,255,24+i*5), width=2)
bg = Image.alpha_composite(bg, overlay)
for name in ('background_960x544.png','background_alt_960x544.png'):
    save(bg, 'fixed/'+name, 'textures', {'role':'VHBB-style wallpaper, no branding'})
header = Image.open(SOURCE/'img_catbar.png').convert('RGBA').resize((960,40),Image.Resampling.LANCZOS)
header = Image.blend(header,Image.new('RGBA',header.size,(13,32,91,255)),0.32)
save(header,'fixed/header_960x40.png','textures')
save(Image.new('RGBA',(128,4),(149,214,255,255)),'fixed/active_indicator_128x4.png','textures')
for name in ('ui_icons_atlas.png','ps_buttons_atlas.png'):
    source = 'themes/dark/'+name
    shutil.copyfile(ROOT/source,BASE/name)
    entry = copy.deepcopy(manifest['sprite_atlases'][source])
    entry['theme'] = 'vhbb'
    manifest['sprite_atlases']['themes/vhbb/'+name] = entry
manifest.setdefault('theme_sources', {})['vhbb'] = {
    'project':'Vita Homebrew Browser', 'author':'devnoname120 and VHBB contributors',
    'url':'https://github.com/devnoname120/vhbb', 'license':'CC BY-NC-SA 4.0',
    'notice':'themes/vhbb/NOTICE.txt',
    'changes':'Panels and header adapted to 9-slice; new unbranded wallpaper; VitaGram icons and fonts.'}
(ROOT/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')


def nine(dst, name, box):
    key = 'themes/vhbb/9slice/'+name+'.png'
    src = Image.open(ROOT/key)
    l,t,r,b = manifest['nine_slice'][key]['insets']
    sw,sh = src.size
    x,y,w,h = box
    for sy,ssh,dy,dh in ((0,t,y,t),(t,sh-t-b,y+t,h-t-b),(sh-b,b,y+h-b,b)):
        for sx,ssw,dx,dw in ((0,l,x,l),(l,sw-l-r,x+l,w-l-r),(sw-r,r,x+w-r,r)):
            if dw>0 and dh>0:
                dst.alpha_composite(src.crop((sx,sy,sx+ssw,sy+ssh)).resize((dw,dh)),(dx,dy))


# Preview composes the actual packaged textures at Vita resolution.
preview = bg.copy()
preview.alpha_composite(header,(0,0))
font_path = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
font = ImageFont.truetype(font_path,20)
small = ImageFont.truetype(font_path,16)
d = ImageDraw.Draw(preview)
d.text((24,7),'VitaGram',font=font,fill=palette['text'])
d.text((795,10),'85%   12:30',font=small,fill=palette['text'])
for i,(label,style) in enumerate((('Все чаты','tab_active'),('Личные','tab_normal'),('Каналы','tab_normal'))):
    nine(preview,style,(24+i*200,52,188,38))
    ImageDraw.Draw(preview).text((39+i*200,60),label,font=small,fill=palette['text'])
for i,(title,text) in enumerate((('Избранное','Сообщения, фотографии и заметки'),('Vita Homebrew','Новые приложения для PS Vita'),('Друзья','Встречаемся вечером?'))):
    y = 104+i*98
    nine(preview,'row_focused' if i==1 else 'row',(24,y,912,86))
    d=ImageDraw.Draw(preview)
    d.rounded_rectangle((36,y+16,90,y+70),radius=12,fill='#2468B8')
    d.text((108,y+13),title,font=font,fill=palette['text'])
    d.text((108,y+47),text,font=small,fill=palette['muted'])
nine(preview,'menu',(24,410,912,92))
d=ImageDraw.Draw(preview)
d.text((44,427),'Настройки → Тема: VHBB',font=font,fill=palette['text'])
d.text((44,463),'Светлая / Тёмная / VHBB',font=small,fill=palette['muted'])
d.text((24,518),'× Открыть    ○ Назад    START Настройки',font=small,fill=palette['text'])
preview.save(BASE/'preview_960x544.png',optimize=True)
print('VHBB theme and preview generated')
