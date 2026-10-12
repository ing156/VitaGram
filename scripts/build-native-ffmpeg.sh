#!/usr/bin/env bash
# Builds the same FFmpeg release, with the same patches, for the host, so that the media tests
# (tests/run-webm-tests.sh, tests/run-video-tests.sh) run against the code the Vita build uses.
# Afterwards: export FFMPEG_NATIVE_BUILD="$PWD/external/ffmpeg-native/build"
set -euo pipefail
cd "$(dirname "$0")/.."
project_dir="$PWD"
source "$project_dir/scripts/ffmpeg-source.sh"
root="$project_dir/external/ffmpeg-native"
source_dir="$root/ffmpeg-$ffmpeg_version"
prepare_ffmpeg_source "$source_dir"
build_dir="$root/build"
rm -rf -- "$build_dir"
mkdir -p "$build_dir"
cd "$build_dir"
# The Vita component set (minus the Vita hardware decoder) plus nothing else; no assembler, so no
# nasm is needed. The tests link the static libraries straight from this build directory.
env -u VITASDK "$source_dir/configure" \
  --disable-shared --enable-static --disable-autodetect --disable-x86asm \
  --disable-programs --disable-doc --disable-debug \
  --disable-everything --disable-network --disable-avdevice --disable-avfilter \
  --enable-avformat --enable-avcodec --enable-avutil --enable-swscale --enable-swresample \
  --enable-decoder=gif,h264,vp8,vp9,opus,aac,mp3,flac,vorbis,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le \
  --enable-encoder=opus,aac \
  --enable-muxer=ogg,mp4 \
  --enable-demuxer=gif,mov,matroska,ogg,mp3,aac,flac,wav \
  --enable-parser=h264,vp8,vp9,opus,aac,mpegaudio,flac,vorbis \
  --enable-bsf=h264_mp4toannexb,extract_extradata \
  --enable-protocol=file --enable-pthreads
make -j"$(nproc)"
printf '\nNative FFmpeg %s built. Run the media tests with:\n  export FFMPEG_NATIVE_BUILD="%s"\n' "$ffmpeg_version" "$build_dir"
