#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# Smaller packaged copies of the largest fonts. Upstream files stay unchanged
# in assets/; CMake packages each result under the upstream name.
#   NotoColorEmoji_b-64.ttf: 64 px emoji strike instead of 136 px.
#   NotoSansJP-JIS.otf:    kanji limited to JIS X 0208, JIS X 0213 plane 1, CP932.
#   NotoSans{KR,SC,TC}-Subset.otf: Hangul (KS X 1001), GB2312, Big5 level 1 + the characters of the .lang file.
jobs=(
    "assets/NotoColorEmoji_b.ttf assets/NotoColorEmoji_b-64.ttf scripts/shrink-emoji-font.py scripts/fetch-emoji-font.sh"
    "assets/NotoSansJP-Regular.otf assets/NotoSansJP-JIS.otf scripts/subset-japanese-font.py scripts/fetch-fallback-fonts.sh"
    "assets/NotoSansKR-Regular.otf assets/NotoSansKR-Subset.otf scripts/subset-cjk-font.py scripts/fetch-fallback-fonts.sh ko assets/lang/ko.lang"
    "assets/NotoSansSC-Regular.otf assets/NotoSansSC-Subset.otf scripts/subset-cjk-font.py scripts/fetch-fallback-fonts.sh zh-Hans assets/lang/zh-Hans.lang"
    "assets/NotoSansTC-Regular.otf assets/NotoSansTC-Subset.otf scripts/subset-cjk-font.py scripts/fetch-fallback-fonts.sh zh-Hant assets/lang/zh-Hant.lang"
)
pending=()
for job in "${jobs[@]}"; do
    read -r source destination tool fetch language langfile <<<"$job"
    if ! test -s "$source"; then
        echo "Missing $source. Run bash $fetch first." >&2
        exit 1
    fi
    if ! { test -s "$destination" && test "$destination" -nt "$source" && test "$destination" -nt "$tool" &&
           { test -z "$langfile" || test "$destination" -nt "$langfile"; }; }; then
        pending+=("$job")
    fi
done
if ((${#pending[@]} == 0)); then exit 0; fi
# Host-only tools, isolated from the system Python.
tools=external/font-tools-venv
if ! "$tools/bin/python" -c "import fontTools, PIL, imagequant" 2>/dev/null; then
    rm -rf -- "$tools"
    if python3 -m venv "$tools" &>/dev/null; then
        :
    else
        # Debian/Ubuntu without python3-venv: bootstrap pip into a bare venv.
        rm -rf -- "$tools"
        python3 -m venv --without-pip "$tools"
        curl --fail --silent --show-error --location --retry 3 https://bootstrap.pypa.io/get-pip.py | "$tools/bin/python" - --quiet
    fi
    "$tools/bin/python" -m pip install --quiet fonttools pillow imagequant
fi
for job in "${pending[@]}"; do
    read -r source destination tool fetch language langfile <<<"$job"
    temporary=$(mktemp "${destination}.tmp.XXXXXX")
    args=()
    if test -n "$language"; then args=("$language" --lang-file "$langfile"); fi
    if "$tools/bin/python" "$tool" "$source" "$temporary" "${args[@]}"; then
        chmod 644 -- "$temporary"
        mv -- "$temporary" "$destination"
    else
        rm -f -- "$temporary"
        exit 1
    fi
done
