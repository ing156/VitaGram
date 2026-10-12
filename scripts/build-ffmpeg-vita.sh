#!/usr/bin/env bash
# Builds FFmpeg 7.1 with the Vita hardware H.264 decoder into external/ffmpeg-vita71/install - the
# FFmpeg the app links by default (CMake option VITAGRAM_FFMPEG=7.1). Nothing is written to the SDK.
set -euo pipefail
: "${VITASDK:?Set VITASDK to your existing VitaSDK directory}"
export PATH="$VITASDK/bin:$PATH"
command -v arm-vita-eabi-gcc >/dev/null
cd "$(dirname "$0")/.."
project_dir="$PWD"
source "$project_dir/scripts/ffmpeg-source.sh"
root="$project_dir/external/ffmpeg-vita71"
source_dir="$root/ffmpeg-$ffmpeg_version"
prepare_ffmpeg_source "$source_dir"
build_dir="$root/build-$ffmpeg_version"
rm -rf -- "$build_dir" "$root/install"
mkdir -p "$build_dir"
cd "$build_dir"
# Same component set as the legacy FFmpeg-vita build; no network, filters, GPL or nonfree code.
"$source_dir/configure" \
  --prefix="$root/install" \
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
mkdir -p "$root/install/share"
cp "$source_dir/COPYING.LGPLv2.1" "$root/install/share/"
printf '\nFFmpeg %s with h264_vita installed in %s\n' "$ffmpeg_version" "$root/install"
