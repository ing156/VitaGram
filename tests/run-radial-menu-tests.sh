#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-radial-tests.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/tests/radial-stubs" -I "$root/src" \
  "$root/tests/radial_menu_test.cpp" -o "$test_dir/radial-menu"
"$test_dir/radial-menu"
