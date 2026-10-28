#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
source "$root/scripts/ffmpeg-source.sh"
ffmpeg_build=${FFMPEG_NATIVE_BUILD:-"$root/external/ffmpeg-native/build"}
ffmpeg_source="$root/external/ffmpeg-native/ffmpeg-$ffmpeg_version"
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-download-stream-tests.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
  -I "$ffmpeg_build" -I "$ffmpeg_source" -I "$root/src" \
  "$root/tests/download_stream_test.cpp" \
  "$ffmpeg_build/libavformat/libavformat.a" "$ffmpeg_build/libavcodec/libavcodec.a" \
  "$ffmpeg_build/libswresample/libswresample.a" \
  "$ffmpeg_build/libavutil/libavutil.a" -lm -o "$test_dir/download-stream"
fixture=${1:-"$root/tests/local/media/El_Ojo_Geek_👁️_on_X_'Hay_peña_que_no_le_está_haciendo_gracia_lo.mp4"}
(cd "$test_dir" && ./download-stream "$fixture")
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread -DVITAGRAM_TEST_SCEIO \
  -I "$root/tests/stream-stubs" -I "$ffmpeg_build" -I "$ffmpeg_source" -I "$root/src" \
  "$root/tests/download_stream_test.cpp" \
  "$ffmpeg_build/libavformat/libavformat.a" "$ffmpeg_build/libavcodec/libavcodec.a" \
  "$ffmpeg_build/libswresample/libswresample.a" "$ffmpeg_build/libavutil/libavutil.a" \
  -lm -o "$test_dir/download-stream-vita"
mkdir "$test_dir/vita"
(cd "$test_dir/vita" && ../download-stream-vita "$fixture")
