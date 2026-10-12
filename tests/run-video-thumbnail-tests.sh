#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-video-thumbnail-tests.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT
# The host build of the FFmpeg release the app ships (scripts/build-native-ffmpeg.sh).
source "$root/scripts/ffmpeg-source.sh"
ffmpeg_source="$root/external/ffmpeg-native/ffmpeg-$ffmpeg_version"
ffmpeg_build=${VITAGRAM_THUMBNAIL_FFMPEG_BUILD:-${FFMPEG_NATIVE_BUILD:-"$root/external/ffmpeg-native/build"}}
if [[ ! -f "$ffmpeg_build/libavformat/libavformat.a" || ! -d "$ffmpeg_source" ]]; then
  echo "Host FFmpeg $ffmpeg_version is missing. Run: bash scripts/build-native-ffmpeg.sh" >&2
  exit 1
fi
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread -DVITAGRAM_VIDEO_THUMBNAILS \
  -I "$ffmpeg_build" -I "$ffmpeg_source" \
  -I "$root/tests/stubs" -I "$root/src" -I "$root/build-modern/external/jansson/include" \
  "$root/src/image_decoder.cpp" "$root/src/vault.cpp" "$root/src/vault_crypto.cpp" \
  "$root/src/secure_store.cpp" "$root/tests/video_thumbnail_test.cpp" \
  "$ffmpeg_build/libavformat/libavformat.a" "$ffmpeg_build/libavcodec/libavcodec.a" \
  "$ffmpeg_build/libswscale/libswscale.a" "$ffmpeg_build/libswresample/libswresample.a" \
  "$ffmpeg_build/libavutil/libavutil.a" \
  -l:libjansson.so.4 -lcrypto -lm -lz $(pkg-config --cflags --libs libpng libjpeg libwebp) \
  -o "$test_dir/thumbnails"
(cd "$test_dir" && ./thumbnails "$@")
