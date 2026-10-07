#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-performance-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
for enabled in 1 0; do
  "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread -DVITAGRAM_PERFORMANCE="$enabled" \
    -DVITAGRAM_TD_TRACE_TEST=1 -I "$root/external/tdlib-upstream/tdutils" \
    -I "$root/src" -I "$root/build-modern/external/jansson/include" \
    "$root/tests/performance_test.cpp" "$root/src/td_diagnostics.cpp" -l:libjansson.so.4 -o "$build_dir/performance-$enabled"
  "$build_dir/performance-$enabled" "$build_dir"
done
