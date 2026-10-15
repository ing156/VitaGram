#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-upload-picker-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers \
  -I "$root/src" -idirafter "${VITASDK:-/usr/local/vitasdk}/arm-vita-eabi/include" \
  -DVITAGRAM_PICKER_LOG=0 "$root/src/upload_picker.cpp" "$root/src/i18n.cpp" \
  "$root/tests/upload_picker_test.cpp" -o "$build_dir/upload_picker_test"
"$build_dir/upload_picker_test"
