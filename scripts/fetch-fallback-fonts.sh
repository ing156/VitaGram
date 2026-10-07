#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p assets
# Pinned Noto sources; each family retains its SIL OFL license.
base=https://raw.githubusercontent.com/google/fonts/23e54b51ddffbc7713c583748e3bd86f62b1fa4a/ofl
fetch() {
    local source="$1" target="$2" temporary
    if test -s "$target"; then return; fi
    temporary=$(mktemp "${target}.download.XXXXXX")
    if curl --fail --location --retry 2 "$base/$source" --output "$temporary"; then
        chmod 644 "$temporary"
        mv -- "$temporary" "$target"
    else
        rm -f -- "$temporary"
        return 1
    fi
}
fetch notosanscoptic/NotoSansCoptic-Regular.ttf assets/NotoSansCoptic-Regular.ttf
fetch notosanscoptic/OFL.txt assets/NotoSansCoptic-LICENSE.txt
fetch notosansmath/NotoSansMath-Regular.ttf assets/NotoSansMath-Regular.ttf
fetch notosansmath/OFL.txt assets/NotoSansMath-LICENSE.txt
fetch notosanssymbols2/NotoSansSymbols2-Regular.ttf assets/NotoSansSymbols2-Regular.ttf
fetch notosanssymbols2/OFL.txt assets/NotoSansSymbols2-LICENSE.txt
# Static Regular avoids the variable font's default Thin (weight 100).
base=https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004
fetch Sans/SubsetOTF/JP/NotoSansJP-Regular.otf assets/NotoSansJP-Regular.otf
fetch LICENSE assets/NotoSansJP-LICENSE.txt
