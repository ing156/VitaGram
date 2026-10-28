"""Exercise the patched FFmpeg receive callback with an asynchronous decoder."""
import os
from pathlib import Path
import subprocess
import sys

root, build = map(Path, sys.argv[1:])
source = (root / 'external/ffmpeg-vita71/ffmpeg-7.1.5/libavcodec/vitadec_video.c').read_text()
start = source.index('static int vita_receive_frame(')
end = source.index('static const AVOption', start)
fixture = r'''
#include <cassert>
#include <cerrno>
#include <cstddef>
#define AVERROR(x) (-(x))
#define AVERROR_EOF (-999)
struct AVCodecContext {};
struct AVFrame { int pts = -1; };
struct AVPacket { int size = 0; };
int inputs, available, drained, released, shown, polls, fail_poll, fail_submit;
bool eof;
int ff_decode_get_packet(AVCodecContext*, AVPacket* p) {
    assert(available == 0); // Never submit more input while pictures are ready.
    if (!inputs) return eof ? AVERROR_EOF : AVERROR(EAGAIN);
    --inputs; p->size = 1; return 0;
}
void av_packet_unref(AVPacket* p) { assert(p->size == 1); p->size = 0; ++released; }
int vita_decode(AVCodecContext*, AVFrame* f, int* got, AVPacket* p) {
    if (!p) { ++polls; if (fail_poll) return -5; }
    else if (p->size) {
        if (fail_submit) return -6;
        // Several pictures become available together after an input delay.
        if (released % 2) available += 3;
    } else if (drained) { --drained; ++available; }
    if (available) { --available; *got = 1; f->pts = shown++; }
    return 0;
}
'''
fixture += source[start:end]
fixture += r'''
int main() {
    AVCodecContext c; AVFrame f;
    inputs = 4;
    for (int i = 0; i < 6; ++i) {
        assert(vita_receive_frame(&c, &f) == 0);
        assert(f.pts == i);
    }
    assert(released == 4 && available == 0 && polls >= 6);
    assert(vita_receive_frame(&c, &f) == AVERROR(EAGAIN));
    available = 2; // Hardware finishes pictures without a new compressed packet.
    assert(vita_receive_frame(&c, &f) == 0 && f.pts == 6);
    assert(vita_receive_frame(&c, &f) == 0 && f.pts == 7);
    assert(released == 4);
    assert(vita_receive_frame(&c, &f) == AVERROR(EAGAIN));
    eof = true; drained = 2;
    assert(vita_receive_frame(&c, &f) == 0 && f.pts == 8);
    assert(vita_receive_frame(&c, &f) == 0 && f.pts == 9);
    assert(vita_receive_frame(&c, &f) == AVERROR_EOF);
    fail_poll = 1;
    assert(vita_receive_frame(&c, &f) == -5);
    fail_poll = 0; fail_submit = 1; inputs = 1;
    assert(vita_receive_frame(&c, &f) == -6 && released == 5);
}
'''
p = build / 'video_decoder_drain.cpp'
p.write_text(fixture)
binary = build / 'video_decoder_drain'
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', str(p), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
print('Hardware receive: delayed/batched pictures, input backpressure, EOF drain and errors: PASS')
