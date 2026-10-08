#!/usr/bin/env python3
"""Subset a Noto Sans CJK font (KR, SC or TC) for the VPK.

Only the characters that language needs are kept; Latin, Cyrillic and the like
come from earlier fonts in the fallback chain.
  ko:      Hangul syllables of KS X 1001, compatibility jamo, CJK punctuation
  zh-Hans: GB2312 (level 1 and 2) plus CJK punctuation
  zh-Hant: Big5 level 1 (5401 common characters) plus CJK punctuation
Every character of the matching assets/lang/<code>.lang is kept as well.
The upstream font in assets/ is not modified.

Requires fontTools.
"""
import argparse

from fontTools import subset
from fontTools.ttLib import TTFont

# (codec, first lead bytes to scan): Hangul syllables only, Big5 level 1 (the 5401 most common characters)
CODECS = {"ko": ("euc_kr", range(0xB0, 0xC9)), "zh-Hans": ("gb2312", range(0xA1, 0xF8)),
          "zh-Hant": ("big5", range(0xA4, 0xC7))}


def codec_characters(codec, leads):
    result = set()
    for first in leads:
        for second in range(0x40, 0xFF):
            try:
                text = bytes((first, second)).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(text) == 1 and ord(text) > 0x7F:
                result.add(ord(text))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source")
    parser.add_argument("destination")
    parser.add_argument("language", choices=sorted(CODECS))
    parser.add_argument("--lang-file", help="also keep every character used by this .lang file")
    args = parser.parse_args()

    wanted = codec_characters(*CODECS[args.language])
    wanted |= set(range(0x3000, 0x303F + 1))      # CJK punctuation
    wanted |= set(range(0xFF00, 0xFFEF + 1))      # full-width forms
    if args.language == "ko":
        wanted |= set(range(0x3130, 0x318F + 1))  # compatibility jamo
    if args.lang_file:
        with open(args.lang_file, encoding="utf-8") as handle:
            wanted |= {ord(c) for c in handle.read() if ord(c) > 0x7F}

    font = TTFont(args.source)
    keep = wanted & set(font.getBestCmap())

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
