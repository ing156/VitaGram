#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-gxm-tests.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  -idirafter "${VITASDK:-/usr/local/vitasdk}/arm-vita-eabi/include" \
  "$root/tests/gxm_guard_test.cpp" \
  -o "$test_dir/gxm-guard"
"$test_dir/gxm-guard"
