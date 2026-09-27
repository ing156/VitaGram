#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-video-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/src" \
  -I "$root/build-modern/external/jansson/include" "$root/tests/video_note_test.cpp" \
  -l:libjansson.so.4 -o "$build_dir/message_test"
"$build_dir/message_test"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread -I "$root/tests/stubs" -I "$root/src" \
  "$root/src/video_note_recorder.cpp" "$root/tests/video_note_recorder_test.cpp" -o "$build_dir/recorder_test"
"$build_dir/recorder_test" "$build_dir"
if [[ -n "${FFMPEG_NATIVE_BUILD:-}" ]]; then
  "${CC:-cc}" -O2 -DH264E_MAX_THREADS=0 -DH264E_SVC_API=0 -I "$root/external/minih264" \
    -c "$root/src/minih264.c" -o "$build_dir/h264.o"
  "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -DH264E_MAX_THREADS=0 -DH264E_SVC_API=0 \
    -I "$root/src" -I "$root/external/minih264" -I "$FFMPEG_NATIVE_BUILD" -I "$FFMPEG_NATIVE_BUILD/src" \
    "$root/src/video_note_encoder.cpp" "$root/tests/video_note_encoder_test.cpp" "$build_dir/h264.o" \
    "$FFMPEG_NATIVE_BUILD/libavformat/libavformat.a" "$FFMPEG_NATIVE_BUILD/libavcodec/libavcodec.a" \
    "$FFMPEG_NATIVE_BUILD/libavutil/libavutil.a" -lm -pthread -o "$build_dir/encoder_test"
  "$build_dir/encoder_test" "$build_dir"
fi
