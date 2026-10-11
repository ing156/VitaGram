#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-secure-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/src" \
  "$root/src/secure_store.cpp" "$root/tests/secure_store_test.cpp" -lcrypto -o "$build_dir/secure_store_test"
"$build_dir/secure_store_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -DVITAGRAM_TEST_NO_ID -I "$root/src" \
  "$root/src/secure_store.cpp" "$root/tests/secure_store_test.cpp" -lcrypto -o "$build_dir/secure_store_noid_test"
"$build_dir/secure_store_noid_test"
