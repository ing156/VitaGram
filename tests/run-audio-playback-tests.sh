#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
ffmpeg_build=${FFMPEG_NATIVE_BUILD:-"$root/external/ffmpeg-native/build"}
source "$root/scripts/ffmpeg-source.sh"
ffmpeg_source="$root/external/ffmpeg-native/ffmpeg-$ffmpeg_version"
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-audio-playback-tests.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/src" -I "$ffmpeg_build" -I "$ffmpeg_source" \
  "$root/tests/audio_playback_test.cpp" "$ffmpeg_build/libavfilter/libavfilter.a" \
  "$ffmpeg_build/libswresample/libswresample.a" "$ffmpeg_build/libavutil/libavutil.a" \
  -lm -pthread -o "$test_dir/audio-playback"
"$test_dir/audio-playback"
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
  -DVITAGRAM_VERSION='"test"' -DVITAGRAM_LANG_DIR='"'"$root/assets/lang"'"' \
  -I "$root/tests/audio-playback-stubs" -I "$root/tests/stubs" -I "$root/src" -I "$root/tests" \
  -I "$root/build-modern/external/jansson/include" -I "$ffmpeg_build" -I "$ffmpeg_source" \
  "$root/src/voice_player.cpp" "$root/src/i18n.cpp" "$root/tests/voice_player_test.cpp" \
  "$ffmpeg_build/libavformat/libavformat.a" "$ffmpeg_build/libavfilter/libavfilter.a" \
  "$ffmpeg_build/libavcodec/libavcodec.a" "$ffmpeg_build/libswresample/libswresample.a" \
  "$ffmpeg_build/libavutil/libavutil.a" -l:libjansson.so.4 -lm -o "$test_dir/voice-player"
"$test_dir/voice-player" "$test_dir" "$@"
