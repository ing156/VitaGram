#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-media-trash-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
for enabled in 1 0; do
  "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread -DVITAGRAM_PERFORMANCE="$enabled" \
    -I "$root/tests/stubs" -I "$root/src" "$root/src/media_trash.cpp" "$root/tests/media_trash_test.cpp" \
    -o "$build_dir/media-trash-$enabled"
  (cd "$build_dir" && rm -rf ux0:data && ./media-trash-"$enabled")
done
