#!/usr/bin/env bash
set -euo pipefail
: "${VITASDK:?Set VITASDK to your existing VitaSDK directory}"
export PATH="$VITASDK/bin:$PATH"
cd "$(dirname "$0")/.."
project_dir="$PWD"
source_dir="$project_dir/external/ffmpeg-vita"
test -f "$source_dir/configure" || {
    echo "Clone https://github.com/fish47/FFmpeg-vita.git (branch vita, commit 3736b0c78da9553b2098dbb0ade0a2e0ddd1753f) into external/ffmpeg-vita" >&2
    exit 1
}
test "$(git -C "$source_dir" rev-parse HEAD)" = 3736b0c78da9553b2098dbb0ade0a2e0ddd1753f || {
    echo "Unexpected FFmpeg-vita revision; use the pinned commit" >&2
    exit 1
}
diagnostic_patch="$project_dir/scripts/ffmpeg-vita-portrait-diagnostics.patch"
if ! git -C "$source_dir" apply --reverse --check "$diagnostic_patch" 2>/dev/null; then
    git -C "$source_dir" apply --check "$diagnostic_patch"
    git -C "$source_dir" apply "$diagnostic_patch"
fi
mkdir -p "$source_dir/build-vitagram"
cd "$source_dir/build-vitagram"
"$source_dir/configure" \
  --prefix="$project_dir/external/ffmpeg-hardware/install" \
  --enable-cross-compile --cross-prefix="$VITASDK/bin/arm-vita-eabi-" \
  --target-os=vita --arch=armv7-a --cpu=cortex-a9 --enable-vita \
  --disable-shared --enable-static --disable-autodetect \
  --disable-runtime-cpudetect --disable-armv5te --disable-armv6t2 \
  --disable-programs --disable-doc --disable-debug --enable-small \
  --disable-everything --disable-network --disable-avdevice --disable-avfilter \
  --enable-avformat --enable-avcodec --enable-avutil --enable-swscale --enable-swresample \
  --enable-decoder=gif,h264,h264_vita,vp8,vp9,opus,aac,mp3,flac,vorbis,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le \
  --enable-encoder=opus,aac \
  --enable-muxer=ogg,mp4 \
  --enable-demuxer=gif,mov,matroska,ogg,mp3,aac,flac,wav \
  --enable-parser=h264,vp8,vp9,opus,aac,mpegaudio,flac,vorbis \
  --enable-bsf=h264_mp4toannexb,extract_extradata \
  --enable-protocol=file \
  --disable-bzlib --disable-iconv --disable-lzma --disable-zlib \
  --enable-pthreads \
  --extra-cflags="-O2 -D_BSD_SOURCE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types" \
  --extra-ldflags="-Wl,-q" --extra-libs="-lpthread -lm"
make -j"$(nproc)"
make install
mkdir -p "$project_dir/external/ffmpeg-hardware/install/share"
cp "$source_dir/COPYING.LGPLv2.1" "$project_dir/external/ffmpeg-hardware/install/share/"
