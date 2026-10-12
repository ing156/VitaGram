#!/usr/bin/env bash
set -euo pipefail
audit_dir=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$audit_dir/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-security-20261012.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
 -I "$root/src" -I "$audit_dir/stubs" -I "$root/build-modern/external/jansson/include" \
 "$audit_dir/recheck.cpp" "$root/src/vault.cpp" "$root/src/vault_crypto.cpp" "$root/src/secure_store.cpp" \
 "$root/src/vault_sweep.cpp" -l:libjansson.so.4 -lcrypto -o "$work/recheck"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$work/recheck" "$work"
