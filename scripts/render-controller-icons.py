#!/usr/bin/env python3
"""Rasterize controller symbols for vita2d without external packages."""
import math
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1] / "assets" / "controller-icons"
SIZE = 64
SAMPLES = 4
BACKGROUND = (102, 102, 102)
COLORS = {
    "triangle": (99, 239, 166),
    "square": (247, 123, 172),
    "cross": (123, 216, 246),
    "circle": (255, 85, 91),
}
ICONS = (*COLORS, "dpad", "start", "select")
FONT = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "C": ("01111", "10000", "10000", "10000", "10000", "10000", "01111"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
}


def inside_triangle(x, y, top, left, right, bottom):
    if y < top or y > bottom:
        return False
    half = (y - top) * (right - left) / (2 * (bottom - top))
    return abs(x - 16) <= half


def distance_to_segment(x, y, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = max(0, min(1, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(x - ax - t * dx, y - ay - t * dy)


def colored(name, x, y):
    if name == "triangle":
        return inside_triangle(x, y, 4, 4, 28, 25) and not inside_triangle(x, y, 9.8, 9, 23, 22)
    if name == "square":
        return 5.5 <= x <= 26.5 and 5.5 <= y <= 26.5 and not (8.8 <= x <= 23.2 and 8.8 <= y <= 23.2)
    if name == "cross":
        return min(distance_to_segment(x, y, 7, 7, 25, 25),
                   distance_to_segment(x, y, 25, 7, 7, 25)) <= 1.6
    radius = math.hypot(x - 16, y - 16)
    return 8.45 <= radius <= 11.55


def label_pixel(label, x, y):
    glyph_scale = 1.45
    left = (56 - (len(label) * 6 - 1) * glyph_scale) / 2
    cx, cy = (x - left) / glyph_scale, (y - 6.9) / glyph_scale
    index = int(cx // 6)
    if 0 <= index < len(label) and 0 <= cy < 7 and 0 <= cx - index * 6 < 5:
        return FONT[label[index]][int(cy)][int(cx - index * 6)] == "1"
    return False


def pixel(name, x, y):
    if name in ("start", "select"):
        def rounded_box(left, top, right, bottom, radius):
            near_x = min(max(x, left + radius), right - radius)
            near_y = min(max(y, top + radius), bottom - radius)
            return (x - near_x) ** 2 + (y - near_y) ** 2 <= radius ** 2
        if not rounded_box(.5, .5, 55.5, 23.5, 3.5):
            return None
        if not rounded_box(1.5, 1.5, 54.5, 22.5, 2.5):
            return (146, 157, 163)
        return (242, 245, 245) if label_pixel(name.upper(), x, y) else (57, 65, 71)
    if (x - 16) ** 2 + (y - 16) ** 2 > 16 ** 2:
        return None
    if name == "dpad":
        radius = math.hypot(x - 16, y - 16)
        if radius > 14: return BACKGROUND
        if radius > 13: return (168, 177, 182)
        cross = (11.5 <= x <= 20.5 and 4 <= y <= 28) or (4 <= x <= 28 and 11.5 <= y <= 20.5)
        if not cross: return (36, 39, 43)
        if (x < 4.8 or x > 27.2 or y < 4.8 or y > 27.2 or
            (11.5 <= x <= 12.4 and (y < 11.5 or y > 20.5)) or
            (19.6 <= x <= 20.5 and (y < 11.5 or y > 20.5)) or
            (11.5 <= y <= 12.4 and (x < 11.5 or x > 20.5)) or
            (19.6 <= y <= 20.5 and (x < 11.5 or x > 20.5))):
            return (193, 202, 208)
        if radius < 2.5: return (32, 39, 44)
        arrows = ((6 <= y <= 10.2 and abs(x - 16) <= (y - 6) * .76) or
                  (21.8 <= y <= 26 and abs(x - 16) <= (26 - y) * .76) or
                  (6 <= x <= 10.2 and abs(y - 16) <= (x - 6) * .76) or
                  (21.8 <= x <= 26 and abs(y - 16) <= (26 - x) * .76))
        return (212, 223, 229) if arrows else (68, 76, 82)
    return COLORS[name] if colored(name, x, y) else BACKGROUND


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def render(name):
    wide = name in ("start", "select")
    width, height = (112, 48) if wide else (SIZE, SIZE)
    rows = []
    for py in range(height):
        row = bytearray([0])
        for px in range(width):
            sums = [0, 0, 0, 0]
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    x = (px + (sx + .5) / SAMPLES) * (56 if wide else 32) / width
                    y = (py + (sy + .5) / SAMPLES) * (24 if wide else 32) / height
                    rgb = pixel(name, x, y)
                    if rgb is None:
                        continue
                    for i in range(3):
                        sums[i] += rgb[i]
                    sums[3] += 255
            count = SAMPLES * SAMPLES
            if sums[3]:
                row.extend(round(channel * 255 / sums[3]) for channel in sums[:3])
                row.append(round(sums[3] / count))
            else:
                row.extend((0, 0, 0, 0))
        rows.append(row)
    data = b"".join(rows)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(data, 9)) + chunk(b"IEND", b"")
    (ROOT / f"{name}.png").write_bytes(png)


if __name__ == "__main__":
    for icon in ICONS:
        render(icon)
