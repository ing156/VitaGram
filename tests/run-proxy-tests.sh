#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-proxy-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/src" -I "$root/tests" -DVITAGRAM_LANG_DIR='"'"$root/assets/lang"'"' \
  "$root/src/proxy_link.cpp" "$root/src/i18n.cpp" "$root/tests/proxy_link_test.cpp" -o "$build_dir/proxy_link_test"
"$build_dir/proxy_link_test"
for source in quirc identify decode version_db; do
  "${CC:-cc}" -std=c99 -O2 -DQUIRC_FLOAT_TYPE=float -DQUIRC_USE_TGMATH \
    -I "$root/external/quirc/lib" -c "$root/external/quirc/lib/$source.c" -o "$build_dir/$source.o"
done
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
  -I "$root/tests/stubs" -I "$root/src" -I "$root/tests" -DVITAGRAM_LANG_DIR='"'"$root/assets/lang"'"' -I "$root/external/quirc/lib" \
  "$root/src/qr_scanner.cpp" "$root/src/proxy_link.cpp" "$root/src/i18n.cpp" "$root/tests/qr_scanner_test.cpp" \
  "$build_dir/quirc.o" "$build_dir/identify.o" "$build_dir/decode.o" "$build_dir/version_db.o" \
  -lm -o "$build_dir/qr_scanner_test"
"$build_dir/qr_scanner_test" "$root/tests/fixtures/mtproxy-qr.txt"
"$build_dir/qr_scanner_test" "$root/tests/fixtures/unrelated-qr.txt" reject
"$build_dir/qr_scanner_test" "$root/tests/fixtures/invalid-proxy-qr.txt" reject
