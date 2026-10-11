#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-photo-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/tests/photo-stubs" -I "$root/src" \
  "$root/tests/photo_view_test.cpp" -o "$build_dir/photo_view_test"
"$build_dir/photo_view_test"
