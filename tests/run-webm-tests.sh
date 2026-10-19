#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
# Media inputs are private and are not shipped with the repository.
if (( $# == 0 )); then
  set -- "$root/assets/sticker.webm" "$root/tests/local/media/alpha.webm"
fi
for fixture in "$@"; do
  if [[ ! -f "$fixture" ]]; then
    echo "Missing media input: $fixture; pass WebM paths as arguments." >&2
    exit 2
  fi
done
: "${FFMPEG_NATIVE_BUILD:?Set to native FFmpeg with matroska, VP9 and swscale}"
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-webm-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/src" \
  -I "$FFMPEG_NATIVE_BUILD" -I "$FFMPEG_NATIVE_BUILD/src" "$root/tests/webm_decode_test.cpp" \
  "$FFMPEG_NATIVE_BUILD/libavformat/libavformat.a" "$FFMPEG_NATIVE_BUILD/libavcodec/libavcodec.a" \
  "$FFMPEG_NATIVE_BUILD/libswscale/libswscale.a" "$FFMPEG_NATIVE_BUILD/libswresample/libswresample.a" \
  "$FFMPEG_NATIVE_BUILD/libavutil/libavutil.a" \
  -lm -pthread -o "$build_dir/webm"
for fixture in "$@"; do
  "$build_dir/webm" "$fixture"
done
