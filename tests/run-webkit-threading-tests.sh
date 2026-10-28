#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-webkit-threads.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
  -I "$root/tests/webkit-stubs" -I "$root/src" \
  "$root/tests/webkit_threading_test.cpp" "$root/src/webview/threading_vita.cpp" \
  -Wl,--wrap=pthread_create -o "$build_dir/threads"
"$build_dir/threads"
