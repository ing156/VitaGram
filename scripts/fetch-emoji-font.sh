#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p assets
# Upstream color bitmap font and its OFL license. Existing assets are preserved.
base=https://raw.githubusercontent.com/googlefonts/noto-emoji/v2.047/fonts
fetch() {
    local source="$1" destination="$2" temporary
    if test -s "$destination"; then return; fi
    temporary=$(mktemp "${destination}.download.XXXXXX")
    if curl --fail --location --retry 3 "$base/$source" --output "$temporary"; then
        mv -- "$temporary" "$destination"
    else
        rm -f -- "$temporary"
        return 1
    fi
}
fetch NotoColorEmoji.ttf assets/NotoColorEmoji.ttf
fetch LICENSE assets/Emoji-LICENSE.txt
