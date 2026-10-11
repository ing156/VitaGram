#!/usr/bin/env python3
"""Offline generator for assets/ui_extra/corner_buttons.png (not needed to build the VPK).

Atlas 144x144, four 72x72 cells: body left, body right (top row), glow left, glow right (bottom row).
Each cell is a quarter of a 66 px radius button whose centre sits in the screen corner (bottom-left /
bottom-right of the 960x544 frame), drawn with 4x supersampling so the rim is antialiased.
The attach glyph is taken from the dark icon atlas of the UI pack; the menu dots are drawn here.
Usage: python3 scripts/build-corner-buttons.py
"""
import json, math, os, struct, sys, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK = os.path.join(ROOT, "assets", "VitaGram_UI_Assets_v1")
OUT = os.path.join(ROOT, "assets", "ui_extra", "corner_buttons.png")
R, CELL, SS = 66.0, 72, 4


def read_png(path):
    data = open(path, "rb").read(); i = 8; idat = b""
    while i < len(data):
        n, t = struct.unpack(">I4s", data[i:i + 8]); c = data[i + 8:i + 8 + n]
        if t == b"IHDR": w, h = struct.unpack(">II", c[:8])
        if t == b"IDAT": idat += c
        i += 12 + n
    raw = zlib.decompress(idat); stride = w * 4; rows = []; prev = bytearray(stride); o = 0
    for _ in range(h):
        f = raw[o]; line = bytearray(raw[o + 1:o + 1 + stride]); o += 1 + stride
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0; b = prev[x]; c = prev[x - 4] if x >= 4 else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line); prev = line
    return w, h, rows


def write_png(path, w, h, rows):
    def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def over(dst, src):  # premultiplied float RGBA
    k = 1 - src[3]
    return [src[0] + dst[0] * k, src[1] + dst[1] * k, src[2] + dst[2] * k, src[3] + dst[3] * k]


def mix(a, b, t): return [a[i] * (1 - t) + b[i] * t for i in range(3)]


def glints(m):  # m: mirrored angle in degrees, as in the original renderer
    return math.exp(-((m + 62) ** 2) / (2 * 14 ** 2)), math.exp(-((m + 14) ** 2) / (2 * 8 ** 2))


def body_sample(x, y):
    d = math.hypot(x, y - CELL); m = math.degrees(math.atan2(y - CELL, x))
    px = [0.0, 0.0, 0.0, 0.0]
    def put(rgb, a): nonlocal px; px = over(px, [rgb[0] / 255 * a, rgb[1] / 255 * a, rgb[2] / 255 * a, a])
    if R < d <= R + 5: put((255, 255, 255), 6 / 255 if d > R + 3 else 15 / 255)
    if d <= R:
        put((30, 34, 40), 150 / 255)
        for i in range(1, 19):
            t = i / 18
            if d <= R * (1 - 0.94 * t): put(mix((62, 68, 78), (150, 158, 170), t), 14 / 255)
        g1, g2 = glints(m)
        base = 70 + 160 * g1 + 110 * g2
        for layer, factor in enumerate((1.0, 0.6, 0.3)):
            if abs(d - (R - 0.5 - layer)) < 0.5: put((255, 255, 255), min(255, base) * factor / 255)
        if abs(d - (R - 7)) < 0.5: put((255, 255, 255), 22 / 255)
    return px


def glow_sample(x, y):
    d = math.hypot(x, y - CELL); m = math.degrees(math.atan2(y - CELL, x))
    if d > R + 2: return [0.0] * 4
    g1, g2 = glints(m)
    ring = min(1.0, (90 + 165 * g1 + 110 * g2) / 255) * max(0.0, 1 - abs(d - (R - 1.5)) / 3.5)
    inner = 0.22 * max(0.0, 1 - d / R) ** 2 if d <= R else 0.0
    a = min(1.0, ring + inner)
    return [a, a, a, a]


def render(sample, mirror):
    rows = []
    for py in range(CELL):
        row = bytearray()
        for px in range(CELL):
            acc = [0.0] * 4
            for sy in range(SS):
                for sx in range(SS):
                    x = px + (sx + 0.5) / SS; y = py + (sy + 0.5) / SS
                    s = sample(CELL - x if mirror else x, y)
                    acc = [acc[i] + s[i] for i in range(4)]
            a = acc[3] / (SS * SS)
            row += bytes([min(255, int(acc[i] / (SS * SS) / a * 255 + 0.5)) for i in range(3)] + [int(a * 255 + 0.5)]) if a > 0 else bytes(4)
        rows.append(row)
    return rows


def blit(rows, sprite, ox, oy, shadow):
    sw, sh, srows = sprite
    for layer in ((1, 1, True), (0, 0, False)):
        for y in range(sh):
            for x in range(sw):
                s = srows[y][x * 4:x * 4 + 4]
                a = s[3] / 255 * (90 / 255 if layer[2] else 1)
                tx, ty = ox + x + layer[0], oy + y + layer[1]
                if a <= 0 or not (0 <= tx < CELL and 0 <= ty < CELL): continue
                rgb = (0, 0, 0) if layer[2] else (s[0], s[1], s[2])
                d = rows[ty][tx * 4:tx * 4 + 4]; da = d[3] / 255; oa = a + da * (1 - a)
                for c in range(3): rows[ty][tx * 4 + c] = int((rgb[c] * a + d[c] * da * (1 - a)) / oa + 0.5)
                rows[ty][tx * 4 + 3] = int(oa * 255 + 0.5)


def dots(rows):
    for i in range(3):
        for shadow in (True, False):
            cx = 36 + i * 11 + (1 if shadow else 0) + 0.0; cy = 48 + (1 if shadow else 0)
            for y in range(int(cy) - 5, int(cy) + 6):
                for x in range(int(cx) - 5, int(cx) + 6):
                    cov = max(0.0, min(1.0, 3.2 + 0.5 - math.hypot(x + 0.5 - cx - 0.5, y + 0.5 - cy - 0.5)))
                    a = cov * (90 / 255 if shadow else 1)
                    if a <= 0 or not (0 <= x < CELL and 0 <= y < CELL): continue
                    rgb = (0, 0, 0) if shadow else (244, 247, 251)
                    d = rows[y][x * 4:x * 4 + 4]; da = d[3] / 255; oa = a + da * (1 - a)
                    for c in range(3): rows[y][x * 4 + c] = int((rgb[c] * a + d[c] * da * (1 - a)) / oa + 0.5)
                    rows[y][x * 4 + 3] = int(oa * 255 + 0.5)


manifest = json.load(open(os.path.join(PACK, "manifest.json")))
atlas_path = "themes/dark/ui_icons_atlas.png"
sp = manifest["sprite_atlases"][atlas_path]["sprites"]["attach"]
aw, ah, arows = read_png(os.path.join(PACK, atlas_path))
glyph = (sp["w"], sp["h"], [arows[sp["y"] + y][sp["x"] * 4:(sp["x"] + sp["w"]) * 4] for y in range(sp["h"])])

left = render(body_sample, False); right = render(body_sample, True)
blit(left, glyph, 25 - sp["w"] // 2, 47 - sp["h"] // 2, True)
dots(right)
glow_l = render(glow_sample, False); glow_r = render(glow_sample, True)
atlas = [bytearray() for _ in range(CELL * 2)]
for y in range(CELL):
    atlas[y] = left[y] + right[y]
    atlas[CELL + y] = glow_l[y] + glow_r[y]
write_png(OUT, CELL * 2, CELL * 2, atlas)
print("wrote", OUT)
