#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-qr-encode-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
for source in quirc identify decode version_db; do
  "${CC:-cc}" -std=c99 -O2 -DQUIRC_FLOAT_TYPE=float -DQUIRC_USE_TGMATH \
    -I "$root/external/quirc/lib" -c "$root/external/quirc/lib/$source.c" -o "$build_dir/$source.o"
done
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/src" -I "$root/external/quirc/lib" \
  "$root/src/qr_encode.cpp" "$root/src/qr_render.cpp" "$root/tests/qr_encode_test.cpp" \
  "$build_dir/quirc.o" "$build_dir/identify.o" "$build_dir/decode.o" "$build_dir/version_db.o" -lm -o "$build_dir/qr_encode_test"
"$build_dir/qr_encode_test"
