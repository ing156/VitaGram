#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-memory-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -I "$root/src" -I "$root/build-modern/external/jansson/include")
"${CXX:-c++}" "${flags[@]}" -O1 -g -fsanitize=address,undefined \
  "$root/src/memory_hygiene.cpp" "$root/src/crash_dump.cpp" "$root/src/process_modules.cpp" \
  "$root/src/vault.cpp" "$root/src/vault_crypto.cpp" "$root/src/secure_store.cpp" \
  "$root/tests/memory_protection_test.cpp" -l:libjansson.so.4 -lcrypto -lz -o "$build_dir/memory_protection"
"$build_dir/memory_protection"
