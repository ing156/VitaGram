#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-transfer-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/src" -I "$root/tests" -DVITAGRAM_LANG_DIR='"'"$root/assets/lang"'"' \
  -I "$root/build-modern/external/jansson/include" "$root/src/i18n.cpp" "$root/tests/upload_status_test.cpp" \
  -l:libjansson.so.4 -o "$build_dir/upload_status"
"$build_dir/upload_status"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra  -pthread -DVITAGRAM_VERSION='"test"' -DVITAGRAM_TEST_NO_ID \
  -I "$root/tests/stubs" -I "$root/src" -I "$root/tests" -DVITAGRAM_LANG_DIR='"'"$root/assets/lang"'"' -I "$root/external/tdlib-upstream" -I "$root/build-modern/tdlib" \
  -I "$root/build-modern/external/jansson/include" \
  "$root/src/telegram_client.cpp" "$root/src/media_cache.cpp" "$root/src/media_trash.cpp" "$root/src/ui_cache.cpp" \
  "$root/src/proxy_link.cpp" "$root/src/i18n.cpp" "$root/src/vault.cpp" "$root/src/vault_crypto.cpp" "$root/src/secure_store.cpp" "$root/src/vault_sweep.cpp" "$root/tests/transfers_test.cpp" -Wl,--wrap=stat -Wl,--wrap=fopen -l:libjansson.so.4 -lcrypto -o "$build_dir/transfers"
(cd "$build_dir" && ./transfers)
if [[ -n "${FFMPEG_AUDIO_BUILD:-}" ]]; then
  "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -pthread \
    -I "$root/tests/stubs" -I "$root/src" -I "$root/tests" -DVITAGRAM_LANG_DIR='"'"$root/assets/lang"'"' -I "$FFMPEG_AUDIO_BUILD" -I "$FFMPEG_AUDIO_BUILD/src" \
    "$root/src/audio_recorder.cpp" "$root/src/i18n.cpp" "$root/tests/audio_recorder_test.cpp" \
    "$FFMPEG_AUDIO_BUILD/libavformat/libavformat.a" "$FFMPEG_AUDIO_BUILD/libavcodec/libavcodec.a" \
    "$FFMPEG_AUDIO_BUILD/libswresample/libswresample.a" "$FFMPEG_AUDIO_BUILD/libavutil/libavutil.a" \
    -lm -o "$build_dir/audio"
  (cd "$build_dir" && ./audio)
fi
