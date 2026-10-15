#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-chat-scroll-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/src" \
  "$root/tests/chat_list_scroll_test.cpp" -o "$build_dir/chat_list_scroll_test"
"$build_dir/chat_list_scroll_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/src" \
  "$root/tests/chat_list_motion_test.cpp" -o "$build_dir/chat_list_motion_test"
"$build_dir/chat_list_motion_test"
