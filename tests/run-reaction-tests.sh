#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-reaction-tests.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/src" \
  "$root/tests/reaction_input_test.cpp" -o "$test_dir/reactions"
"$test_dir/reactions"

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/src" \
  "$root/tests/quote_layout_test.cpp" -o "$test_dir/quotes"
"$test_dir/quotes"
