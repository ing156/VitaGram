#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-vault-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -I "$root/src" -I "$root/build-modern/external/jansson/include")
"${CXX:-c++}" "${flags[@]}" -O2 "$root/src/vault_crypto.cpp" "$root/tests/vault_crypto_test.cpp" -lcrypto -o "$build_dir/vault_crypto"
"$build_dir/vault_crypto"
# Same code under AddressSanitizer / UBSan.
"${CXX:-c++}" "${flags[@]}" -O1 -g -fsanitize=address,undefined \
  "$root/src/vault.cpp" "$root/src/vault_crypto.cpp" "$root/src/secure_store.cpp" "$root/tests/vault_test.cpp" \
  -l:libjansson.so.4 -lcrypto -o "$build_dir/vault"
"$build_dir/vault"
"${CXX:-c++}" "${flags[@]}" -O1 -g -fsanitize=address,undefined -pthread -I "$root/tests/stubs" \
  "$root/src/vault.cpp" "$root/src/vault_crypto.cpp" "$root/src/secure_store.cpp" "$root/src/vault_sweep.cpp" \
  "$root/tests/vault_sweep_test.cpp" -l:libjansson.so.4 -lcrypto -o "$build_dir/vault_sweep"
"$build_dir/vault_sweep"
"${CXX:-c++}" "${flags[@]}" -O1 -g -fsanitize=address,undefined -pthread \
  "$root/src/vault.cpp" "$root/src/vault_crypto.cpp" "$root/src/secure_store.cpp" "$root/src/vault_readahead.cpp" \
  "$root/tests/vault_readahead_test.cpp" -l:libjansson.so.4 -lcrypto -o "$build_dir/vault_readahead"
"$build_dir/vault_readahead"
