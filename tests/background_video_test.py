"""Run production video-note routing with fake decoders; no Vita/GPU required."""
from pathlib import Path
import os
import subprocess
import sys

root, build = map(Path, sys.argv[1:])
main = (root / 'src/main.cpp').read_text()


def section(start, end):
    begin = main.index(start)
    return main[begin:main.index(end, begin)]


fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace i18n { using Text = std::string; }
namespace ui_skin { int polls = 0; void reload_palette() { ++polls; } }
struct Stream { std::string source; bool ready() const { return true; } };
namespace download_stream { using Shared = std::shared_ptr<Stream>; }
std::string tr(const char* text) { return text; }
struct AnimationPlayer {
    std::string source;
    bool video = false, paused = true, failed = false;
    int generation = 0, volume = 100, uploads = 0;
    int64_t position = 0;
    void setSource(std::string path, bool v = false, bool = false, bool = false,
                   bool = false, download_stream::Shared = nullptr, int32_t = 0) {
        if (path == source && v == video) return;
        source = path; video = v; paused = v; position = 0; ++generation;
    }
    void toggle() { if (!source.empty() && video) paused = !paused; }
    bool playing() const { return !source.empty() && video && !paused; }
    bool failedToOpen() const { return failed; }
    int64_t positionUs() const { return position; }
    void upload() { ++uploads; if (playing()) position += 10000; }
    bool canSeek() const { return !source.empty() && video; }
    int64_t seekLimitUs() const { return 10000000; }
    void seekTo(int64_t target) { position = target; }
    void setVolume(int value) { volume = value; }
    double positionSeconds() const { return position / 1e6; }
    int64_t seekLimitUs(int64_t limit) const { return limit; }
    void setPlaying(bool play) { paused = !play; }
};
struct MessageItem {
    bool video_note = true, video = true, animation = true;
    int64_t id = 1;
    int video_duration = 60, animation_file_id = 101, voice_file_id = 202;
    std::string animation_error, animation_path = "note.mp4";
};
struct Client {
    std::vector<MessageItem> items{MessageItem{}};
    auto& messages() { return items; }
    void clearAudioQueue() {}
} tg;
struct Active { bool open = false; bool active() const { return open; } } upload_picker, kb, recorder, circle_recorder;
AnimationPlayer animation, video_note_animation, voice;
bool post_open = true, bg_autoplay = false, prev_anim_playing = false;
int message_index = 0, bg_volume = 100, bg_unmuted_volume = 100;
'''
fixture += section('    struct BackgroundMedia {', '    bool bg_autoplay =')
fixture += 'int main() {\n'
fixture += section('    auto post_animation =', '    auto invalidate_spoiler_view =')
fixture += section('    auto bg_duration =', '    auto bg_switch_track =')
fixture += r'''
    bool settings_open = false, photo_open = false, stickers_open = false, gifs_open = false, running = true;
    auto spoiler_locked = [](const MessageItem&) { return false; };
    auto selected_index = [&] {
'''
fixture += section('        int animation_index =', '        tg.setAnimationTarget(animation_index,')
fixture += r'''
        return animation_index;
    };
    struct Preview { bool animated = true; std::string full_path = "preview.gif", path = "upload.mp4"; } preview;
    const Preview* preview_sticker = nullptr;
    const Preview* preview_gif = nullptr;
    const Preview* picker_focus = &preview;
    bool picker_video = false, unsupported_video = false, prepare_video = false;
    download_stream::Shared animation_stream;
    auto frame = [&](const MessageItem* animation_message, bool toggle_video = false) {
'''
fixture += section('        AnimationPlayer& media_animation =', '        auto animation_stream =')
fixture += section('        // Navigation and silent previews', '        if (portrait_play_pending &&')
fixture += section('        if (toggle_video && (animation_message || picker_video))', '        int voice_index =')
fixture += r'''
    };
    auto arbitrate = [&] {
        const bool animation_playing = animation.playing() || video_note_animation.playing();
'''
fixture += section('        // One sound output:', '        if (voice_message) {\n            const bool same')
fixture += r'''
    };
    auto* note = &tg.items[0];
    frame(note, true);
    assert(bg_media.live && bg_media.circle && bg_media.file == 101);
    assert(video_note_animation.playing() && &post_animation() == &video_note_animation);
    const int generation = video_note_animation.generation;
    // Settings, keyboard, photos and pickers select no post animation.
    for (bool* menu : {&settings_open, &kb.open, &photo_open, &stickers_open, &gifs_open}) {
        *menu = true;
        assert(selected_index() == -1);
        animation.setSource("");
        frame(nullptr);
        assert(video_note_animation.playing() && bg_media.live);
        assert(video_note_animation.generation == generation);
        *menu = false;
    }
    post_open = false; assert(selected_index() == -1); frame(nullptr);
    // Foreground previews remain independent, including paused upload video.
    preview_gif = &preview; frame(nullptr);
    assert(animation.source == preview.full_path && video_note_animation.playing());
    preview_gif = nullptr; preview_sticker = &preview; frame(nullptr);
    assert(video_note_animation.playing());
    preview_sticker = nullptr; picker_video = true; frame(nullptr);
    assert(animation.source == preview.path && video_note_animation.playing());
    frame(note);
    assert(animation.source == preview.path && video_note_animation.playing());
    picker_video = false;
    assert(video_note_animation.generation == generation);
    bg_seek(4000000); bg_set_volume(35);
    assert(video_note_animation.position == 4000000 && bg_position() == 4000000);
    assert(video_note_animation.volume == 35 && animation.volume == 35 && voice.volume == 35);
    // Browsing another note does not take over until Play, even with the same message ID.
    post_open = true;
    note->animation_file_id = 303; note->animation_path = "another-note.mp4";
    frame(note);
    assert(bg_media.file == 101 && video_note_animation.source == "note.mp4");
    assert(&post_animation() == &animation);
    frame(note, true);
    assert(bg_media.file == 303 && video_note_animation.source == "another-note.mp4");
    assert(video_note_animation.playing() && &post_animation() == &video_note_animation);
    assert(animation.source.empty());
    const int second_generation = video_note_animation.generation;
    post_open = false; frame(nullptr); post_open = true; frame(note);
    assert(video_note_animation.generation == second_generation && video_note_animation.playing());
    // Merely viewing a voice message cannot replace the background note.
    MessageItem selected_voice;
    const MessageItem* voice_message = &selected_voice;
    bool toggle_voice = false, history_voice_toggle = false;
'''
fixture += section('        if (voice_message && bg_media.live && (bg_media.circle', '        if (diag_log::enabled(diag_log::Kind::Performance))')
fixture += r'''
    assert(!voice_message);
    // The newly started sound source owns the output.
    voice.setSource("music.ogg", true); voice.toggle(); prev_anim_playing = false;
    arbitrate(); assert(!voice.playing() && video_note_animation.playing());
    voice.toggle(); arbitrate(); assert(voice.playing() && !video_note_animation.playing());
    voice.setSource(""); video_note_animation.toggle();
    MessageItem clip; clip.video_note = false; clip.animation_path = "clip.mp4";
    frame(&clip, true);
    assert(animation.playing() && !video_note_animation.playing());
    // Paused background notes survive navigation; natural completion retires the bar.
    frame(nullptr); assert(bg_media.live);
    video_note_animation.position = 60000000;
    frame(nullptr); assert(!bg_media.live);
    for (bool video_playing : {true, false, true, false}) {
        const int before = ui_skin::polls;
'''
fixture += section('        if (!video_playing) ui_skin::reload_palette();', '\n        bool dark =')
fixture += r'''
        assert(ui_skin::polls == before + (video_playing ? 0 : 1));
    }
}
'''
source = build / 'background_video_test.cpp'
source.write_text(fixture)
binary = build / 'background_video_test'
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                '-Werror', str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
print('Background video: navigation, previews, source identity, controls, audio ownership and deferred palette polling: PASS')

# Exercise the production quality menu and resume gate independently of Vita/GPU code.
quality_fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
namespace ui_skin { enum class Icon { Check }; }
struct Variant { int width, height, file; std::string path; };
struct MessageItem {
    bool video = true, video_note = false;
    int64_t id = 66;
    int animation_file_id = 102;
    std::string animation_path, animation_error;
    std::vector<Variant> video_variants{{320,180,101,""}, {640,360,102,""}};
};
struct ChatMenuItem { const char* label; unsigned button; int special, icon = -1; std::string text = {}; };
struct Player {
    bool failed = false, ready = false, seekable = true;
    int64_t limit = 0, position = 0;
    bool failedToOpen() const { return failed; }
    bool readyForPlayback() const { return ready; }
    bool canSeek() const { return seekable; }
    int64_t seekLimitUs() const { return limit; }
    void seekTo(int64_t time) { position = time; }
} media_animation;
struct Client { int64_t chat = 42; int64_t currentChatId() const { return chat; } } tg;
int main() {
    std::vector<MessageItem> m(1);
    bool video_quality_menu = true, has_message = true;
    int message_index = 0;
    auto menu = [&] {
        std::vector<ChatMenuItem> v;
'''
quality_fixture += section('        if (video_quality_menu &&', '        if (recorder.active())')
quality_fixture += r'''
        return v;
    };
    auto items = menu();
    assert(items.size() == 3 && items[0].text == "320 × 180" && items[1].text == "640 × 360");
    assert(items[0].icon == -1 && items[1].icon == (int)ui_skin::Icon::Check && items.back().special == 42);
    m[0].video_variants.clear();
    items = menu(); assert(items.size() == 2 && items[0].special == 3 && items.back().special == 42);
    has_message = false; assert(menu().empty()); has_message = true;
    bool toggle_video = false, quality_resume_playing = true;
    int64_t quality_resume_chat = 42, quality_resume_message = 66, quality_resume_time = 5000000;
    int quality_resume_file = 102;
    const MessageItem* animation_message = &m[0];
    auto restore = [&] {
'''
quality_fixture += section('        if (toggle_video) quality_resume_message = 0;', '        if (toggle_video && (animation_message || picker_video))')
quality_fixture += r'''
    };
    restore(); assert(quality_resume_message == 66 && !toggle_video);
    media_animation.ready = true; restore(); assert(quality_resume_message == 66);
    media_animation.limit = 5000000; restore();
    assert(!quality_resume_message && toggle_video && media_animation.position == 5000000);
    toggle_video = false; quality_resume_playing = false; quality_resume_message = 66;
    restore(); assert(!quality_resume_message && !toggle_video && media_animation.position == 5000000);
    quality_resume_message = 66; quality_resume_time = 0; media_animation.seekable = false;
    restore(); assert(!quality_resume_message);
    quality_resume_message = 66; tg.chat = 99; restore(); assert(!quality_resume_message);
    tg.chat = 42; quality_resume_message = 66; m[0].animation_file_id = 103;
    restore(); assert(!quality_resume_message);
    m[0].animation_file_id = 102; quality_resume_message = 66; media_animation.failed = true;
    restore(); assert(!quality_resume_message);
    media_animation.failed = false; quality_resume_message = 66; animation_message = nullptr;
    restore(); assert(!quality_resume_message);
    quality_resume_message = 66; animation_message = &m[0]; toggle_video = true;
    restore(); assert(!quality_resume_message); // Explicit play/pause overrides a pending automatic resume.
}
'''
source = build / 'player_quality_test.cpp'
source.write_text(quality_fixture)
binary = build / 'player_quality_test'
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                '-Werror', str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
print('Player quality: menu rows, chosen badge, empty/error state, resume position, pause and cancellation: PASS')

# Exercise the real video clock across a long progressive-read stall.
player_source = (root / 'src/animation_player.cpp').read_text()
clock_start = player_source.index('    auto video_time =')
clock_end = player_source.index('    video_diagnostics::Playback diagnostic_playback;', clock_start)
read_start = player_source.index('        const int64_t read_started =', clock_end)
read_end = player_source.index('        if (rc == AVERROR_EOF)', read_start)
clock_fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
int64_t tick = 1000000;
int64_t sceKernelGetSystemTimeWide() { return tick; }
int av_read_frame(void*, void*) { tick += 3000000; return 0; }
struct Audio {
    bool ready = false, done = false;
    std::atomic<bool> buffering{false};
    bool finished() const { return done; }
    bool clockReady() const { return ready; }
    std::atomic<bool>& videoBuffering() { return buffering; }
    int64_t positionUs() const { return 1234567; }
} audio_;
int main() {
    int64_t video_wall = 0, tail_wall = 0, tail_position = 0, audio_wait_started = 0, seek_base = 0;
    bool wall_fallback = false;
    FILE* hardware_log = nullptr;
'''
clock_fixture += player_source[clock_start:clock_end]
clock_fixture += r'''
    assert(video_time() == -1 && !wall_fallback);
    void* format = nullptr; void* packet = nullptr; int rc = -1;
'''
clock_fixture += player_source[read_start:read_end]
clock_fixture += r'''
    assert(rc == 0 && audio_wait_started == 0);
    assert(video_time() == -1 && !wall_fallback);
    audio_.ready = true;
    assert(video_time() == 1234567 && !wall_fallback);
}
'''
source = build / 'video_clock_test.cpp'
source.write_text(clock_fixture)
binary = build / 'video_clock_test'
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                '-Werror', str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
print('Video clock: input stalls do not trigger the audio startup fallback: PASS')

# A frozen audio clock must not prevent the next decoded picture from releasing the stall.
wait_start = player_source.index('    auto wait_video =')
wait_end = player_source.index('    auto present_frame =', wait_start)
wait_fixture = r"""
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdint>
struct Audio { std::atomic<bool> stalled{true}; std::atomic<bool>& videoFrameStalled() { return stalled; } } audio_;
struct Reservation { void reset() {} void emplace(int) {} } reservation;
namespace cpu_scheduler { struct Role { static constexpr int Video = 1; }; }
int64_t tick = 0; int64_t sceKernelGetSystemTimeWide() { return tick; }
void sceKernelDelayThread(int us) { tick += us; }
int interrupted(void*) { return tick > 100000; }
int main() {
    int job = 0; bool round = false, had_frame = true, paused_ = false, video = true;
    int64_t seek_base = 0, video_wall = 0, tail_wall = 0;
    struct { int64_t due = -1; } diagnostic_playback;
    int snapshots = 0;
    auto decoder_stage = [&](const char*) { ++snapshots; };
    auto video_time = [] { return int64_t(1000000); };
"""
wait_fixture += player_source[wait_start:wait_end]
wait_fixture += r"""
    assert(wait_video(2000000) == 2000000 && tick == 0);
    audio_.stalled = false;
    assert(wait_video(1000000) == 1000000);
    assert(diagnostic_playback.due == 1000000 && snapshots == 2);
    wait_video(2000000);
    assert(snapshots > 2 && diagnostic_playback.due == 2000000);
}
"""
source = build / 'video_stall_clock_test.cpp'
source.write_text(wait_fixture)
binary = build / 'video_stall_clock_test'
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
print('Video stall: next frame can resume a paused audio clock: PASS')

# Exercise the production stall trigger, including pause, seek startup and low frame rates.
trigger_start = player_source.index('bool AnimationPlayer::buffering() const {')
trigger_end = player_source.index('\ni18n::Text AnimationPlayer::status()', trigger_start)
trigger = player_source[trigger_start:trigger_end]
trigger_line = next(line for line in player_source.splitlines() if 'audio_.videoFrameStalled() = !awaiting_frame_' in line)
trigger_fixture = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
int64_t tick = 1000000;
int64_t sceKernelGetSystemTimeWide() { return tick; }
struct Audio { std::atomic<bool> stalled{false}; bool done = false;
    std::atomic<bool>& videoFrameStalled() { return stalled; } bool finished() const { return done; } };
struct AnimationPlayer {
    bool video_ = true; std::string source_ = "video";
    std::atomic<bool> playback_ready_{true}, finished_{false}, awaiting_frame_{false}, paused_{false};
    std::atomic<int64_t> start_us_{0}, last_frame_us_{1000000}, frame_step_us_{40000};
    Audio audio_;
    bool buffering() const;
    void update() {
'''
trigger_fixture += trigger_line + '\n    }\n};\n' + trigger
trigger_fixture += r'''
int main() {
    AnimationPlayer player;
    player.update(); assert(!player.audio_.stalled);
    tick += 800000; player.update(); assert(player.audio_.stalled);
    player.last_frame_us_ = tick; player.update(); assert(!player.audio_.stalled);
    tick += 800000; player.paused_ = true; player.update(); assert(!player.audio_.stalled);
    player.paused_ = false; player.awaiting_frame_ = true; player.update(); assert(!player.audio_.stalled);
    player.awaiting_frame_ = false; player.frame_step_us_ = 1000000; player.update(); assert(!player.audio_.stalled);
    tick += 2300000; player.update(); assert(player.audio_.stalled);
    player.audio_.done = true; player.update(); assert(!player.audio_.stalled);
}
'''
source = build / 'video_stall_trigger_test.cpp'
source.write_text(trigger_fixture)
binary = build / 'video_stall_trigger_test'
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
print('Video stall trigger: frame arrival, pause, seek startup, low frame rate and audio end: PASS')

# Reproduce the device log: 251 old packets without a picture, then normal output.
player_source = (root / 'src/animation_player.cpp').read_text()
guard_start = player_source.index('        // Lost or non-picture packets')
guard_end = player_source.index('        decoder_stage("read_packet");', guard_start)
guard = player_source[guard_start:guard_end]
reset_line = next(line for line in player_source.splitlines() if 'frames_decoded != frames_before_packet' in line)
throttle_fixture = r'''
#include <cassert>
#include <cstdint>
unsigned frames_decoded = 0, packets_without_frame = 0, delays = 0;
bool video = true, direct_video = true, output = false;
int job = 0, frame_error = 0;
int interrupted(void*) { return 0; }
void sceKernelDelayThread(int) { ++delays; }
void decoder_stage(const char*) {}
void fail(int) { assert(false); }
bool receive() { if (output) ++frames_decoded; return true; }
void feed() {
'''
throttle_fixture += guard
throttle_fixture += '''
    const unsigned frames_before_packet = frames_decoded;
    ++packets_without_frame;
    if (!receive()) { fail(frame_error); return; }
'''+reset_line+'''
}
int main() {
    for (int i = 0; i < 251; ++i) feed();
    assert(packets_without_frame == 251 && delays == 0);
    output = true;
    feed();
    assert(delays == 1 && packets_without_frame == 0);
    for (int i = 0; i < 100; ++i) feed();
    assert(delays == 1 && packets_without_frame == 0);
    output = false;
    for (int i = 0; i < 251; ++i) feed();
    feed();
    assert(delays == 401 && packets_without_frame == 1);
}
'''
source = build / 'video_throttle_test.cpp'
source.write_text(throttle_fixture)
binary = build / 'video_throttle_test'
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
print('Hardware packet throttle: old losses do not delay each subsequent frame; no-output retry is bounded: PASS')

# The restart-from-zero fallback is only for a seek that never produced a picture.
player_source = (root / 'src/animation_player.cpp').read_text()
line = next(line.strip() for line in player_source.splitlines() if 'start_us_.load() > 0 && !prepare && !had_frame' in line)
condition = line[len('if ('):-len(') {')]
fallback_fixture = r'''
#include <atomic>
#include <cassert>
int job = 0, cancelled = 0;
bool interrupted(void*) { return cancelled; }
std::atomic<long long> start_us_{10000000};
bool video = true, prepare = false, had_frame = false;
bool should_restart() { return ''' + condition + r'''; }
int main() {
    assert(should_restart());
    had_frame = true; assert(!should_restart());
    had_frame = false; start_us_ = 0; assert(!should_restart());
    start_us_ = 10000000; cancelled = 1; assert(!should_restart());
    cancelled = 0; prepare = true; assert(!should_restart());
}
'''
source = build / 'video_seek_failure_test.cpp'
source.write_text(fallback_fixture)
binary = build / 'video_seek_failure_test'
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
print('Seek failure: no jump to the start after playback has produced a frame: PASS')

# Run the production packet-pacing call site with fake clocks and cancellation.
player_source = (root / 'src/animation_player.cpp').read_text()
pace_start = player_source.index('            // Keep the hardware ES buffer bounded')
pace_end = player_source.index('            ++packets_read;', pace_start)
pace_fixture = r'''
#include "video_packet_pacer.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <climits>
constexpr int64_t AV_NOPTS_VALUE = INT64_MIN;
constexpr int AV_TIME_BASE_Q = 1;
struct Packet { int64_t dts = 6000000; } packet_value, *packet = &packet_value;
struct Stream { int time_base = 1; } stream_value, *stream = &stream_value;
struct State { bool complete = true; } stream_state;
struct Audio { std::atomic<bool> stalled{false}, lagging{false}; std::atomic<bool>& videoFrameStalled() { return stalled; } std::atomic<bool>& videoFrameLagging() { return lagging; } } audio_;
VideoPacketPacer packet_pacer;
bool video = true, direct_video = true, download_value = true, *download = &download_value;
bool paused_ = false, had_frame = true, cancel = false, dynamic_clock = true, unref = false;
int64_t tick = 1000000, timeline_origin_us = 0, seek_base = 0;
int job = 0;
int64_t av_rescale_q(int64_t x, int, int) { return x; }
int64_t av_sat_sub64(int64_t a, int64_t b) { return a-b; }
int64_t sceKernelGetSystemTimeWide() { return tick; }
void sceKernelDelayThread(int64_t n) { tick += n; }
int64_t video_time() { return dynamic_clock ? tick - 1000000 : 0; }
bool interrupted(void*) { return cancel && tick >= 1010000; }
void av_packet_unref(Packet*) { unref = true; }
void decoder_stage(const char*) {}
unsigned frames_decoded = 1, packets_without_frame = 0;
int frame_error = 0;
bool receive() { return true; }
void fail(int) { assert(false); }
void feed() { while (true) {
'''
pace_fixture += player_source[pace_start:pace_end]
pace_fixture += r'''
    break;
} }
int main() {
    stream_state.complete = false; feed(); assert(tick >= 3000000);
    tick = 1000000;
    stream_state.complete = true; had_frame = false; dynamic_clock = false;
    feed(); assert(tick == 1000000);
    dynamic_clock = true; paused_ = true; seek_base = 1000000;
    feed(); assert(tick == 1000000);
    paused_ = false; had_frame = true; seek_base = 0;
    feed(); assert(tick >= 3000000 && !unref);
    packet_pacer = VideoPacketPacer{}; // A fresh playback generation owns a fresh pacer.
    tick = 1000000; dynamic_clock = false; audio_.lagging = true;
    feed(); assert(tick >= 3000000 && !unref);
    audio_.lagging = false;
    packet_pacer = VideoPacketPacer{};
    tick = 1000000; dynamic_clock = false; cancel = true;
    feed(); assert(unref && tick >= 1010000);
}
'''
source = build / 'video_packet_pacing_integration_test.cpp'
source.write_text(pace_fixture)
binary = build / 'video_packet_pacing_integration_test'
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'src'), str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
print('Local video pacing: streaming bypass, paused seek preview, clock gating and cancellation: PASS')

# Exercise the production audio-lead gate across playback transitions.
player_source = (root / 'src/animation_player.cpp').read_text()
lag_start = player_source.index('    const bool sync = video_')
lag_end = player_source.index('\n    pthread_mutex_lock(&mutex_);', lag_start)
lag_fixture = r'''
#include "video_sync.h"
#include <atomic>
#include <cassert>
#include <cstdint>
struct Audio {
    bool done = false, ready = true; int64_t clock = 1000000; std::atomic<bool> lagging{false};
    bool finished() const { return done; } bool clockReady() const { return ready; }
    int64_t positionUs() const { return clock; }
    std::atomic<bool>& videoFrameLagging() { return lagging; }
} audio_;
bool video_ = true, round_ = false, prepare_ = false, paused_ = false, finished_ = false, awaiting_frame_ = false;
int64_t position_us_ = 1000000, frame_step_us_ = 41708;
void update() {
'''
lag_fixture += player_source[lag_start:lag_end]
lag_fixture += r'''
}
int main() {
    update(); assert(!audio_.lagging);
    audio_.clock += 200000; update(); assert(audio_.lagging);
    position_us_ += 150000; update(); assert(!audio_.lagging);
    position_us_ = 1000000; update(); assert(audio_.lagging);
    paused_ = true; update(); assert(!audio_.lagging);
    paused_ = false; awaiting_frame_ = true; update(); assert(!audio_.lagging);
    awaiting_frame_ = false; round_ = true; update(); assert(!audio_.lagging);
    round_ = false; audio_.ready = false; update(); assert(!audio_.lagging);
    audio_.ready = true; audio_.done = true; update(); assert(!audio_.lagging);
}
'''
source = build / 'video_audio_lead_test.cpp'
source.write_text(lag_fixture)
binary = build / 'video_audio_lead_test'
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'src'), str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
assert 'had_frame && !audio_.videoFrameLagging() && clock - video_due > 100000' in player_source
print('Audio lead: catch-up, pause, seek, video notes and audio end: PASS')

# Exercise the production source-selection guard across TDLib/cache updates.
guard_start = player_source.index('    if (video_ == video && round_ == round', player_source.index('void AnimationPlayer::setSource'))
guard_end = player_source.index('\n    video_ = video;', guard_start)
source_fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
using Shared = std::shared_ptr<int>;
int mutex_ = 0;
void pthread_mutex_unlock(int*) {}
bool video_ = false, round_ = false, prepare_ = false, cache_only_ = false;
std::atomic<bool> finished_{false};
std::string source_; Shared stream_; int32_t source_file_id_ = 0; int generation = 0;
void setSource(const std::string& path, bool video, bool round, bool prepare, bool cache_only, Shared stream, int32_t file_id) {
'''
source_fixture += player_source[guard_start:guard_end]
source_fixture += r'''
    video_ = video; round_ = round; prepare_ = prepare; cache_only_ = cache_only; ++generation;
}
int main() {
    auto growing = std::make_shared<int>(1);
    setSource("stream", true, false, false, false, growing, 10);
    assert(generation == 1);
    setSource("cached.mp4", true, false, false, false, nullptr, 10);
    setSource("", true, false, false, false, nullptr, 10);
    assert(generation == 1 && source_ == "stream" && stream_ == growing);
    setSource("other.mp4", true, false, false, false, nullptr, 11);
    assert(generation == 2 && source_ == "other.mp4");
    setSource("", false, false, false, false, nullptr, 0);
    setSource("other.mp4", true, false, false, false, nullptr, 11);
    assert(generation == 4);
    finished_ = true;
    setSource("stream", true, false, false, false, growing, 11);
    assert(generation == 5 && source_ == "stream");
    finished_ = false;
    setSource("other.mp4", true, false, true, false, nullptr, 11);
    assert(generation == 6);
}
'''
source = build / 'video_source_stability_test.cpp'
source.write_text(source_fixture)
binary = build / 'video_source_stability_test'
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
print('Video source: cache changes, quality, reopen, failure and preparation: PASS')
