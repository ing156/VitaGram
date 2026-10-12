#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-gif-tests.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/src" -I "$root/build-modern/external/jansson/include" \
  "$root/tests/gif_catalog_test.cpp" -l:libjansson.so.4 -o "$work/gif_catalog_test"
"$work/gif_catalog_test"
