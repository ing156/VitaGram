#!/usr/bin/env python3
"""Downscale the color emoji bitmap strike for the VPK.

The UI draws emoji at roughly 22-31 px (28 px * text scale), while upstream
Noto Color Emoji stores 136x128 PNG bitmaps. A smaller strike keeps a 2x
margin, shrinks the package and the per-emoji texture memory. The upstream
font in assets/ is not modified.

Requires fontTools, Pillow and imagequant (libimagequant, as used upstream
via pngquant).
"""
import argparse
import io
import struct
import sys
import zlib

import imagequant
from fontTools.ttLib import TTFont
from PIL import Image


def scaled(value, factor):
    return int(round(value * factor))


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))


def palette_png(image):
    """Compact indexed PNG: only used colors, translucent entries first."""
    rgba = image.convert("RGBA")
    width, height = rgba.size
    data = rgba.tobytes()
    pixels = [tuple(data[i:i + 4]) for i in range(0, len(data), 4)]
    colors = sorted(set(pixels), key=lambda c: (c[3] == 255, c))
    index = {color: i for i, color in enumerate(colors)}
    depth = next(bits for bits in (1, 2, 4, 8) if len(colors) <= 1 << bits)
    per_byte = 8 // depth
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # Filter none: best for small indexed images.
        row = pixels[y * width:(y + 1) * width]
        for x in range(0, width, per_byte):
            packed = 0
            for i in range(per_byte):
                value = index[row[x + i]] if x + i < width else 0
                packed |= value << (8 - depth * (i + 1))
            rows.append(packed)
    translucent = sum(1 for c in colors if c[3] != 255)
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, 3, 0, 0, 0)) +
            chunk(b"PLTE", b"".join(bytes(c[:3]) for c in colors)) +
            (chunk(b"tRNS", bytes(c[3] for c in colors[:translucent])) if translucent else b"") +
            chunk(b"IDAT", zlib.compress(bytes(rows), 9)) +
            chunk(b"IEND", b""))


def shrink(source, destination, target, colors):
    font = TTFont(source)
    cblc, cbdt = font["CBLC"], font["CBDT"]
    if len(cblc.strikes) != 1:
        sys.exit(f"expected one bitmap strike, found {len(cblc.strikes)}")
    size = cblc.strikes[0].bitmapSizeTable
    if size.hori.widthMax <= target:
        sys.exit(f"strike is already {size.hori.widthMax}px wide; use the upstream font as the source")
    factor = target / size.hori.widthMax

    for glyph in cbdt.strikeData[0].values():
        if not hasattr(glyph, "imageData"):  # Also triggers lazy decompilation.
            sys.exit(f"unsupported CBDT glyph {type(glyph).__name__}")
        image = Image.open(io.BytesIO(glyph.imageData)).convert("RGBA")
        width = max(1, scaled(image.width, factor))
        height = max(1, scaled(image.height, factor))
        # Resample premultiplied alpha: straight RGBA darkens transparent edges.
        small = image.convert("RGBa").resize((width, height), Image.LANCZOS).convert("RGBA")
        quantized = imagequant.quantize_pil_image(small, dithering_level=0.0, max_colors=colors,
                                                  min_quality=0, max_quality=100)
        glyph.imageData = palette_png(quantized)
        metrics = glyph.metrics
        metrics.width, metrics.height = width, height
        metrics.BearingX = scaled(metrics.BearingX, factor)
        metrics.BearingY = scaled(metrics.BearingY, factor)
        metrics.Advance = scaled(metrics.Advance, factor)

    size.ppemX = scaled(size.ppemX, factor)
    size.ppemY = scaled(size.ppemY, factor)
    for line in (size.hori, size.vert):
        line.ascender = scaled(line.ascender, factor)
        line.descender = scaled(line.descender, factor)
        line.widthMax = scaled(line.widthMax, factor)
    font.save(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source")
    parser.add_argument("destination")
    parser.add_argument("--size", type=int, default=64, help="target bitmap width in pixels (default 64)")
    parser.add_argument("--colors", type=int, default=64, help="maximum palette entries per glyph (default 64)")
    args = parser.parse_args()
    shrink(args.source, args.destination, args.size, args.colors)


if __name__ == "__main__":
    main()
