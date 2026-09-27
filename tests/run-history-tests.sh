#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-history-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/src" \
  -I "$root/build-modern/external/jansson/include" \
  "$root/tests/history_selection_test.cpp" -o "$build_dir/history_selection_test"
"$build_dir/history_selection_test"
