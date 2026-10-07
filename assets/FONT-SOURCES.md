# Packaged fallback fonts

Unmodified upstream font files. SIL Open Font License text is packaged alongside each font.

- `NotoSansCoptic-Regular.ttf`: Google Fonts commit `23e54b51ddffbc7713c583748e3bd86f62b1fa4a`, `ofl/notosanscoptic/`. SHA-256: `51990af1bb0df7be8bfe98fda97769cecfa36eee15381e5504df400276abd735`.
- `NotoSansJP-Regular.otf`: [Noto CJK Sans 2.004](https://github.com/notofonts/noto-cjk/tree/Sans2.004), `Sans/SubsetOTF/JP/NotoSansJP-Regular.otf`. SHA-256: `dff723ba59d57d136764a04b9b2d03205544f7cd785a711442d6d2d085ac5073`.

The Japanese font is static Regular. This avoids both the larger variable font and its default weight of 100 (Thin).
Fetch script: `scripts/fetch-fallback-fonts.sh`. Existing Noto Sans and color emoji assets remain in use.

- `NotoSansMath-Regular.ttf`: Google Fonts commit `23e54b51ddffbc7713c583748e3bd86f62b1fa4a`, `ofl/notosansmath/`. SHA-256: `3f495fe933c06786e4d5f6d86b8ee70b6753a68ee3b9d87528726de0f6e2c47d`. SIL OFL in `NotoSansMath-LICENSE.txt`; covers mathematical alphabets including bold Fraktur.

- `NotoSansSymbols2-Regular.ttf`: Google Fonts commit `23e54b51ddffbc7713c583748e3bd86f62b1fa4a`, `ofl/notosanssymbols2/`. SHA-256: `7d5fb73b7ca67a6798101741f5d280a3d016a56a197afcd4199dbb57b4b82a21`. SIL OFL in `NotoSansSymbols2-LICENSE.txt`; includes U+2727 (✧).

- `NotoColorEmoji-64.ttf` (generated, not committed): derived from the upstream `NotoColorEmoji.ttf` by `scripts/shrink-fonts.sh`. The CBDT strike is resampled from 136 px to 64 px and each glyph is re-quantized to an indexed PNG with at most 64 colors; the metrics are scaled to match. It is packaged as `assets/NotoColorEmoji.ttf` under the same SIL OFL 1.1 (`Emoji-LICENSE.txt`); the license declares no Reserved Font Name.
- `NotoSansJP-JIS.otf` (generated, not committed): subset of the upstream `NotoSansJP-Regular.otf` by `scripts/shrink-fonts.sh` (fontTools). All non-Han code points are kept; Han characters are limited to JIS X 0208, JIS X 0213 plane 1 and the Windows CP932 NEC/IBM extensions. Glyph outlines and OpenType layout data of the kept characters are unchanged. Packaged as `assets/NotoSansJP-Regular.otf` under the same SIL OFL 1.1 (`NotoSansJP-LICENSE.txt`).
