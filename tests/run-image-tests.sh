#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-image-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
  -I "$root/tests/stubs" -I "$root/src" \
  "$root/src/image_decoder.cpp" "$root/tests/image_decoder_test.cpp" \
  $(pkg-config --cflags --libs libpng libjpeg libwebp) -o "$build_dir/images"
(cd "$build_dir" && ./images)
