#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${TD_NATIVE_TEST_BUILD:-/tmp/vitagram-td-native}
cmake -S "$root/external/tdlib-upstream" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-DTD_VITA_COOPERATIVE=1
cmake --build "$build_dir" --target tdactor -j2
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -pthread \
  -I "$root/external/tdlib-upstream/tdactor" -I "$root/external/tdlib-upstream/tdutils" \
  -I "$build_dir/tdutils" "$root/tests/td_scheduler_test.cpp" \
  "$build_dir/tdactor/libtdactor.a" "$build_dir/tdutils/libtdutils.a" \
  -lssl -lcrypto -lz -ldl -o "$build_dir/vitagram-scheduler-test"
"$build_dir/vitagram-scheduler-test"
