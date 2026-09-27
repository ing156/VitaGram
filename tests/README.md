# Native checks

`bash tests/run-history-tests.sh` checks stable selection in mixed photo/video albums,
navigation in both directions, prepended history pages, photo-album merges and deletions.
It uses the application message type and selection code; a configured build-modern
Jansson include directory is required.

`bash tests/run-proxy-tests.sh` checks proxy URLs, real QR decoding and mocked Vita camera lifecycle.

`bash tests/run-video-tests.sh` checks circular YUV cropping, the current TDLib
`inputMessageVideoNote` payload, camera switching, microphone failure, cancellation,
the 60-second recording limit and transfer of file ownership to the upload caller.
It requires C/C++ compilers, pthreads, a host libjansson.so.4, and the Jansson headers
from a configured build-modern tree. Vita camera/GPU/audio calls are replaced with
stubs. The recorder's encoder is stubbed only in its lifecycle test.

To also run the **real** minih264 + AAC + MP4 encoder/decoder test, build host FFmpeg
in a separate directory (do not replace the Vita libraries):

```bash
mkdir -p /tmp/vitagram-video-ffmpeg-test
cd /tmp/vitagram-video-ffmpeg-test
/path/to/vitagram/external/ffmpeg-animation/ffmpeg-7.1.5/configure \
  --disable-all --disable-autodetect --disable-doc --disable-programs --disable-x86asm \
  --enable-avcodec --enable-avformat --enable-avutil --enable-encoder=aac \
  --enable-decoder=h264,aac --enable-muxer=mp4 --enable-demuxer=mov \
  --enable-parser=h264,aac --enable-protocol=file
make -j4
cd /path/to/vitagram
FFMPEG_NATIVE_BUILD=/tmp/vitagram-video-ffmpeg-test bash tests/run-video-tests.sh
```

The test decodes every video and audio packet and checks square dimensions,
circle masking, non-silent audio, duration, monotonic timestamps (including a gap
between camera frames), cancellation and truncated input cleanup.

These tests do not verify the physical Vita camera, microphone, GPU, encoder
performance or delivery to Telegram. Test on a device: open a chat → L → X;
switch with Triangle; X to stop; Square to play; X to send. Verify front/rear video,
audio synchronization, cancel/reopen, the 60-second limit, and the destination topic.
