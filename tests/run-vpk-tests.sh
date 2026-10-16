#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-vpk-tests.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I "$root/src" "$root/src/vpk_package.cpp" "$root/tests/vpk_package_test.cpp" -lcrypto -o "$work/vpk"
"$work/vpk"
