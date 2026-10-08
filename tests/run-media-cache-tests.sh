#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-media-cache-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
  -I "$root/tests/stubs" -I "$root/src" -I "$root/build-modern/external/jansson/include" \
  "$root/src/media_cache.cpp" "$root/src/vault.cpp" "$root/src/vault_crypto.cpp" "$root/src/secure_store.cpp" "$root/tests/media_cache_test.cpp" \
  -Wl,--wrap=stat -l:libjansson.so.4 -lcrypto -o "$build_dir/media-cache"
(cd "$build_dir" && ./media-cache)
