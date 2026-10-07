#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${TD_NATIVE_TEST_BUILD:-/tmp/vitagram-td-native}
cmake -S "$root/external/tdlib-upstream" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-DTD_VITA_COOPERATIVE=1
cmake --build "$build_dir" --target tdutils -j2
# Compile the actual Vita stderr branch; wrap only its OS write boundary.
"${CXX:-c++}" -std=c++17 -O2 -pthread -DTD_VITA_STDERR_BEST_EFFORT=1 \
  -I "$root/external/tdlib-upstream/tdutils" -I "$build_dir/tdutils" \
  "$root/tests/td_logging_test.cpp" "$root/external/tdlib-upstream/tdutils/td/utils/TsCerr.cpp" \
  "$build_dir/tdutils/libtdutils.a" -Wl,--wrap=write -lssl -lcrypto -lz -ldl \
  -o "$build_dir/vitagram-logging-test"
"$build_dir/vitagram-logging-test"
