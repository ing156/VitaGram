#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
: "${FFMPEG_NATIVE_BUILD:?Set to native FFmpeg with matroska, VP9 and swscale}"
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-webm-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/src" \
  -I "$FFMPEG_NATIVE_BUILD" -I "$FFMPEG_NATIVE_BUILD/src" "$root/tests/webm_decode_test.cpp" \
  "$FFMPEG_NATIVE_BUILD/libavformat/libavformat.a" "$FFMPEG_NATIVE_BUILD/libavcodec/libavcodec.a" \
  "$FFMPEG_NATIVE_BUILD/libswscale/libswscale.a" "$FFMPEG_NATIVE_BUILD/libswresample/libswresample.a" \
  "$FFMPEG_NATIVE_BUILD/libavutil/libavutil.a" \
  -lm -pthread -o "$build_dir/webm"
"$build_dir/webm" "${1:-$root/assets/sticker.webm}"
