#!/usr/bin/env bash
# Generates the TDLib sources that the Vita cross build needs but that TDLib does not keep in git
# (code generated from the TL schemas, MIME tables). They are built on the host, once per TDLib
# revision, into external/tdlib-upstream/*/generate/auto.
set -euo pipefail
cd "$(dirname "$0")/.."
project_dir="$PWD"
td="$project_dir/external/tdlib-upstream"
test -f "$td/CMakeLists.txt" || { echo "external/tdlib-upstream is missing. Run: git submodule update --init" >&2; exit 1; }
for tool in cmake c++ gperf; do
    command -v "$tool" >/dev/null || { echo "$tool is required on the host" >&2; exit 1; }
done
# The same patched tree the Vita build uses (CMake applies the patch as well; this is idempotent).
if ! git -C "$td" apply --reverse --check "$project_dir/scripts/tdlib-vita.patch" 2>/dev/null; then
    git -C "$td" apply "$project_dir/scripts/tdlib-vita.patch"
fi
build="$project_dir/build-native-modern"
# A host build: no Vita toolchain, whatever VITASDK says.
env -u VITASDK cmake -S "$td" -B "$build" -DCMAKE_BUILD_TYPE=Release -DTD_ENABLE_TESTS=OFF
cmake --build "$build" --target prepare_cross_compiling --parallel "$(nproc)"
printf '\nTDLib sources generated in %s\n' "$td"
