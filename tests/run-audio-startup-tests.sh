#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
if (( $# == 0 )); then
  echo 'Usage: bash tests/run-audio-startup-tests.sh /path/to/music.mp3 /path/to/music.m4a' >&2
  exit 2
fi
ffmpeg_build=${FFMPEG_NATIVE_BUILD:-"$root/external/ffmpeg-native/build"}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-audio-startup-tests.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
  -I "$root/src" -I "$ffmpeg_build" -I "$ffmpeg_build/src" \
  "$root/tests/audio_startup_test.cpp" \
  "$ffmpeg_build/libavformat/libavformat.a" "$ffmpeg_build/libavcodec/libavcodec.a" \
  "$ffmpeg_build/libswresample/libswresample.a" "$ffmpeg_build/libavutil/libavutil.a" \
  -lm -o "$test_dir/audio-startup"
"$test_dir/audio-startup" "$@"
