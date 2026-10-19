#!/usr/bin/env bash
# Host test for the TGS pre-check that runs before rlottie (src/tgs_policy.h).
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
fixture=${1:-"$root/tests/local/media/AnimatedSticker.tgs"}
if [[ ! -f "$fixture" ]]; then
  echo "Missing media input: $fixture; pass a TGS path as the first argument." >&2
  exit 2
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-tgs-policy.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I "$root/src" \
  -I "$root/build-modern/external/jansson/include" "$root/tests/tgs_policy_test.cpp" \
  -l:libjansson.so.4 -lz -o "$work/tgs_policy_test"
shopt -s nullglob
"$work/tgs_policy_test" "$fixture" "$root"/external/rlottie-vita/source/example/resource/*.json
