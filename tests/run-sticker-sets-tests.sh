#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-sticker-sets-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  -I "$root/src" -I "$root/build-modern/external/jansson/include" \
  "$root/tests/sticker_sets_test.cpp" -l:libjansson.so.4 -o "$build_dir/sticker-sets"
"$build_dir/sticker-sets"
