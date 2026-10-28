#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-gxm-tests.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  -idirafter "${VITASDK:-/usr/local/vitasdk}/arm-vita-eabi/include" \
  "$root/tests/gxm_guard_test.cpp" \
  -o "$test_dir/gxm-guard"
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -idirafter "${VITASDK:-/usr/local/vitasdk}/arm-vita-eabi/include" \
  "$root/src/vita2d_texture_guard.cpp" "$root/tests/vita2d_texture_guard_test.cpp" \
  -o "$test_dir/texture-guard"
mkdir -p "$test_dir/ux0:data/vitagram"
cd "$test_dir"
"$test_dir/gxm-guard"
"$test_dir/texture-guard"
