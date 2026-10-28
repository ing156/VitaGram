#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-storage-probe.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread -I "$root/src" \
  "$root/tests/storage_probe_test.cpp" -o "$build_dir/test"
"$build_dir/test" "$build_dir"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread -D__vita__ \
  -I "$root/tests/storage-probe-stubs" -I "$root/src" \
  "$root/tests/storage_probe_test.cpp" -o "$build_dir/test-vita"
"$build_dir/test-vita" "$build_dir"
