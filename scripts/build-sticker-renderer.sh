#!/usr/bin/env bash
set -euo pipefail
: "${VITASDK:?Set VITASDK}"
cd "$(dirname "$0")/.."
root="$PWD/external/rlottie-vita"
compatibility_script="$PWD/scripts/patch-rlottie.cmake"
compatibility_header="$PWD/scripts/rlottie-vita-compat.h"
mkdir -p "$root"
# rlottie is a pinned git submodule (.gitmodules); VitaGram's changes are scripts/rlottie-vita.patch
# (polygon/star point limits) and patch-rlottie.cmake (static loader, no embedded images).
if ! test -e "$root/source/.git"; then
    echo "external/rlottie-vita/source is missing. Run: git submodule update --init" >&2
    exit 1
fi
git -C "$root/source" rev-parse HEAD
if ! git -C "$root/source" apply --reverse --check "$PWD/scripts/rlottie-vita.patch" 2>/dev/null; then
    git -C "$root/source" apply "$PWD/scripts/rlottie-vita.patch"
fi
cmake -DSOURCE_FILE="$root/source/src/vector/vimageloader.cpp" -P "$compatibility_script"
cmake -S "$root/source" -B "$root/build" \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DARCH:STRING=arm \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_C_FLAGS:STRING="-D_GNU_SOURCE -D_DEFAULT_SOURCE -include \"$compatibility_header\"" \
    -DCMAKE_CXX_FLAGS:STRING="-D_GNU_SOURCE -D_DEFAULT_SOURCE -include \"$compatibility_header\"" \
    -DCMAKE_INSTALL_PREFIX="$root/install" -DLIB_INSTALL_DIR="$root/install/lib" \
    -DBUILD_SHARED_LIBS=OFF -DLOTTIE_MODULE=OFF -DLOTTIE_THREAD=OFF \
    -DLOTTIE_CACHE=OFF -DLOTTIE_TEST=OFF
cmake --build "$root/build" -j"$(nproc)"
cmake --install "$root/build"
mkdir -p "$root/install/share"
cp "$root/source/COPYING" "$root/install/share/rlottie-COPYING"
mkdir -p "$root/install/share/rlottie-licenses"
cp -R "$root/source/licenses/." "$root/install/share/rlottie-licenses/"
