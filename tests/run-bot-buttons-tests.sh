#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-bot-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/src" -I /usr/include/freetype2 \
  -I "$root/external/jansson/include" -I "$root/external/jansson/src" \
  "$root/tests/bot_buttons_test.cpp" -l:libjansson.so.4 -l:libfreetype.so.6 -o "$build_dir/bots"
"$build_dir/bots" "$root/assets"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/tests/font-stubs" -I "$root/src" \
  -I "$root/external/jansson/include" -I "$root/external/jansson/src" \
  "$root/tests/webapp_view_test.cpp" -l:libjansson.so.4 -o "$build_dir/webapp"
(cd "$build_dir" && ./webapp)
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread -I "$root/src" \
  "$root/tests/webapp_trace_test.cpp" "$root/src/webview/trace.cpp" -o "$build_dir/trace"
(cd "$build_dir" && ./trace)
node "$root/tests/webapp_bridge_test.js"
