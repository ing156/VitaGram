#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-keyboard-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I "$root/tests/keyboard-stubs" -I "$root/src" -idirafter "${VITASDK:-/usr/local/vitasdk}/arm-vita-eabi/include")
"${CXX:-c++}" "${flags[@]}" "$root/src/keyboard.cpp" "$root/src/custom_keyboard.cpp" "$root/src/keyboard_case.cpp" "$root/src/keyboard_layout.cpp" "$root/src/i18n.cpp" "$root/tests/custom_keyboard_test.cpp" -o "$build_dir/custom_keyboard"
"$build_dir/custom_keyboard" "$root/assets/lang"
"${CXX:-c++}" "${flags[@]}" "$root/tests/keyboard_hold_test.cpp" -o "$build_dir/keyboard_hold"
"$build_dir/keyboard_hold"

"${CXX:-c++}" "${flags[@]}" "$root/src/keyboard_layout.cpp" "$root/tests/keyboard_layout_test.cpp" -o "$build_dir/keyboard_layout"
"$build_dir/keyboard_layout" "$root/assets/lang"

"${CXX:-c++}" "${flags[@]}" "$root/src/keyboard_case.cpp" "$root/tests/keyboard_case_test.cpp" -o "$build_dir/keyboard_case"
"$build_dir/keyboard_case"
