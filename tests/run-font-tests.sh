#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-font-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
headers=${FREETYPE_INCLUDE_DIR:-/usr/include/freetype2}
if [[ ! -f "$headers/ft2build.h" ]]; then headers="${VITASDK:-/usr/local/vitasdk}/arm-vita-eabi/include/freetype2"; fi
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/src" -I "$headers" \
  "$root/tests/font_fallback_test.cpp" "$root/src/utf8_decode.cpp" -l:libfreetype.so.6 -o "$build_dir/fonts"
"$build_dir/fonts" "$root/assets"
