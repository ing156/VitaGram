#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-cpu-scheduler.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -U_FORTIFY_SOURCE -Wall -Wextra -Werror -Wno-error=stringop-truncation -pthread \
  -I "$root/tests/cpu-scheduler-stubs" -I "$root/src" \
  "$root/src/cpu_scheduler.cpp" "$root/tests/cpu_scheduler_test.cpp" \
  -Wl,--wrap=fprintf -o "$build_dir/test"
"$build_dir/test"
