#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-ui-mask-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/src" -I "$root/build-modern/external/jansson/include" \
  "$root/tests/ui_mask_test.cpp" -l:libjansson.so.4 -o "$build_dir/ui-mask"
"$build_dir/ui-mask"
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/src" -I "$root/build-modern/external/jansson/include" \
  "$root/tests/ui_themes_test.cpp" -l:libjansson.so.4 -o "$build_dir/ui-themes"
"$build_dir/ui-themes"
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/tests/ui-skin-stubs" -I "$root/src" -I "$root/build-modern/external/jansson/include" \
  "$root/src/ui_skin.cpp" "$root/tests/ui_skin_test.cpp" -l:libjansson.so.4 -lpng -o "$build_dir/ui-skin"
"$build_dir/ui-skin" "$root/assets/VitaGram_UI_Assets_v1" "$build_dir"
