#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${TD_NATIVE_TEST_BUILD:-/tmp/vitagram-td-native}
cmake -S "$root/external/tdlib-upstream" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$build_dir" --target tdutils --parallel "$(nproc)"
"${CXX:-c++}" -std=c++17 -O2 -pthread -DVITAGRAM_TD_SCEIO_TEST=1 -DVITAGRAM_TD_TRACE_TEST=1 \
  -I "$root/tests/stream-stubs" -I "$root/external/tdlib-upstream/tdutils" -I "$build_dir/tdutils" \
  "$root/tests/td_positioned_io_test.cpp" "$root/external/tdlib-upstream/tdutils/td/utils/port/FileFd.cpp" \
  "$build_dir/tdutils/libtdutils.a" -lssl -lcrypto -lz -ldl -o "$build_dir/vitagram-positioned-test"
"$build_dir/vitagram-positioned-test"
python3 "$root/tests/binlog_batching_test.py" "$root" "$build_dir"
