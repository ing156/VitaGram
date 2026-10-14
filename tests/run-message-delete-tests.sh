#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-delete-tests.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/src" \
  "$root/tests/message_delete_policy_test.cpp" -o "$work/test"
"$work/test"
