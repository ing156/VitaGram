"""Run the production packet-wait loop with delayed hardware pictures."""
from pathlib import Path
import os
import subprocess
import sys
root, build = map(Path, sys.argv[1:])
source = (root / 'src/animation_player.cpp').read_text()
start = source.index('            // Keep the hardware ES buffer bounded')
end = source.index('            ++packets_read;', start)
fixture = r'''
#include "video_packet_pacer.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
constexpr int64_t AV_NOPTS_VALUE = INT64_MIN;
constexpr int AV_TIME_BASE_Q = 1;
int64_t av_rescale_q(int64_t value, int, int) { return value; }
int64_t av_sat_sub64(int64_t a, int64_t b) { return a - b; }
struct Packet { int64_t dts = 7007000; };
struct Stream { int time_base = 1; };
void av_packet_unref(Packet*) {}
int64_t tick, audio_clock;
int polls, delivered;
bool cancelled, error, failed;
int64_t sceKernelGetSystemTimeWide() { return tick; }
void sceKernelDelayThread(int us) { tick += us; if (tick > 5000000) cancelled = true; }
bool interrupted(void*) { return cancelled; }
struct Audio {
    bool stalled = true;
    bool videoFrameStalled() { return stalled; }
    bool videoFrameLagging() { return false; }
} audio_;
void run(bool paused_, bool had_frame) {
    bool video = true, direct_video = true;
    int job = 0; int64_t seek_base = paused_ ? 1000000 : 0, timeline_origin_us = 0;
    Packet held; Packet* packet = &held; Stream st; Stream* stream = &st;
    VideoPacketPacer packet_pacer;
    unsigned frames_decoded = 2, packets_without_frame = 170;
    std::string frame_error = "hardware error";
    auto fail = [&](const std::string&) { failed = true; };
    auto decoder_stage = [](const char*) {};
    auto video_time = [] { return audio_clock; };
    auto receive = [&] {
        ++polls;
        if (error) return false;
        if (tick >= 1025000) {
            ++frames_decoded; ++delivered; audio_clock = 4000000; audio_.stalled = false;
        }
        return true;
    };
    for (int once = 0; once < 1; ++once) {
'''
fixture += source[start:end]
fixture += r'''
    }
    (void)packets_without_frame;
}
int main() {
    tick = 1000000; audio_clock = 474666;
    run(false, true);
    assert(polls > 0 && delivered == 1 && !failed && !cancelled);
    assert(tick < 1100000); // Ready pictures release the hold without seconds of virtual-clock advance.
    tick = 1000000; audio_clock = 0; polls = delivered = 0;
    run(true, false); // A paused seek still allows its preview without polling/playing audio.
    assert(polls == 0 && tick == 1000000);
    tick = 1000000; audio_clock = 0; error = true;
    run(false, true); assert(failed);
    failed = false; error = false; cancelled = true; polls = 0;
    run(false, true); assert(polls == 0 && !failed);
}
'''
p = build / 'video_packet_wait.cpp'
p.write_text(fixture)
binary = build / 'video_packet_wait'
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I', str(root / 'src'), str(p), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
print('Packet wait: delayed pictures unblock stalled audio, paused preview, cancellation and decoder errors: PASS')
