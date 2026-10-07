#!/usr/bin/env python3
"""Subset the Japanese fallback font for the VPK.

Every non-Han code point of the upstream font is kept (kana, punctuation,
full-width forms, symbols). Han characters are limited to JIS X 0208 and
JIS X 0213 plane 1 (levels 1-3) and the CP932 extensions, which cover
modern Japanese text including names. Rarer kanji and Chinese-only hanzi of
the upstream font are dropped.
The upstream font in assets/ is not modified.

Requires fontTools.
"""
import argparse

from fontTools import subset
from fontTools.ttLib import TTFont


def is_han(code):
    return (0x3400 <= code <= 0x4DBF or 0x4E00 <= code <= 0x9FFF or
            0xF900 <= code <= 0xFAFF or 0x20000 <= code <= 0x3FFFF)


def jis_characters():
    """JIS X 0208 (EUC-JP), JIS X 0213 plane 1 (EUC-JIS-2004) and the Windows
    CP932 NEC/IBM extensions, which hold common name kanji such as U+9AD9."""
    result = set()
    ranges = {"euc_jp": (range(0xA1, 0xFF), range(0xA1, 0xFF)),
              "euc_jis_2004": (range(0xA1, 0xFF), range(0xA1, 0xFF)),
              "cp932": ([*range(0x81, 0xA0), *range(0xE0, 0xFD)], range(0x40, 0xFD))}
    for codec, (leads, trails) in ranges.items():
        for first in leads:
            for second in trails:
                try:
                    result.update(ord(c) for c in bytes((first, second)).decode(codec))
                except UnicodeDecodeError:
                    pass
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source")
    parser.add_argument("destination")
    args = parser.parse_args()

    font = TTFont(args.source)
    jis = jis_characters()
    keep = {code for code in font.getBestCmap() if not is_han(code) or code in jis}

    options = subset.Options()
    options.layout_features = ["*"]
    options.name_IDs = ["*"]
    options.name_languages = ["*"]
    options.notdef_outline = True
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=keep)
    subsetter.subset(font)
    font.save(args.destination)


if __name__ == "__main__":
    main()
