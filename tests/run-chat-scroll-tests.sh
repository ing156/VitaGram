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
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/src" \
  "$root/tests/chat_view_layout_test.cpp" -o "$build_dir/chat_view_layout_test"
"$build_dir/chat_view_layout_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/src" \
  "$root/tests/folder_drawer_test.cpp" -o "$build_dir/folder_drawer_test"
"$build_dir/folder_drawer_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/src" \
  "$root/tests/history_scroll_test.cpp" -o "$build_dir/history_scroll_test"
"$build_dir/history_scroll_test"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/src" \
  "$root/tests/chat_bubble_test.cpp" -o "$build_dir/chat_bubble_test"
"$build_dir/chat_bubble_test"
python3 "$root/tests/chat_modal_test.py"
python3 "$root/tests/settings_menu_test.py"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/src" \
  "$root/tests/chat_calendar_test.cpp" -o "$build_dir/chat_calendar_test"
"$build_dir/chat_calendar_test"
