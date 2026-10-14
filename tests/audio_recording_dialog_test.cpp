#include "audio_recording_dialog.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cstdio>

struct Signals {
    inline static bool fail_start = false, failed_capture = false, running = false, encoding = false, complete = false, playing = false;
    inline static int starts = 0, peak = 15, level = 10;
};
struct Capture {
    std::string owned;
    bool start(const std::string& path) {
        ++Signals::starts;
        if (Signals::fail_start) return false;
        owned = path; Signals::running = true; return true;
    }
    void stop() { Signals::running = false; Signals::encoding = !Signals::failed_capture; }
    bool recording() const { return Signals::running; }
    bool processing() const { return Signals::encoding; }
    std::string takeCompleted() {
        if (!Signals::complete) return {};
        Signals::complete = false; Signals::encoding = false;
        std::ofstream(owned) << "OggS";
        auto result = owned; owned.clear(); return result;
    }
    void cancel() {
        Signals::running = Signals::encoding = Signals::complete = false;
        if (!owned.empty()) std::remove(owned.c_str());
        owned.clear();
    }
    int seconds() const { return 2; }
    int conversionPercent() const { return 100; }
    int peak() const { return Signals::peak; }
    int level() const { return Signals::level; }
    void setStatus(const std::string&) {}
    std::string status() const { return "capture"; }
};
struct Playback {
    ~Playback() { Signals::playing = false; }
    void setSource(const std::string& path) { assert(std::filesystem::exists(path)); }
    void toggle() { Signals::playing = !Signals::playing; }
    void setPlaying(bool playing) { Signals::playing = playing; }
    bool playing() const { return Signals::playing; }
    double positionSeconds() const { return 1; }
    std::string error() const { return {}; }
};
using Dialog = AudioRecordingDialog<Capture, Playback>;
using State = Dialog::State;

static void prepare(Dialog& dialog, const char* path) {
    dialog.open(path);
    const int starts = Signals::starts;
    assert(dialog.active() && dialog.state() == State::Preview && dialog.seconds() == 0);
    dialog.pump(100); assert(Signals::starts == starts); // Opening never captures audio.
    dialog.record(); assert(dialog.state() == State::Recording);
    dialog.pump(100); assert(dialog.levels().back() == 30);
    Signals::peak = 100; dialog.pump(50100); assert(dialog.levels().back() == 100);
    Signals::peak = 15;
    dialog.finish(); assert(dialog.state() == State::Encoding && dialog.path().empty());
    dialog.pump(100100); assert(dialog.state() == State::Encoding); // No automatic send or review before encoding finishes.
    Signals::complete = true; dialog.pump(150100);
    assert(dialog.state() == State::Review && dialog.path() == path && std::filesystem::exists(path));
}
int main() {
    Dialog dialog;
    prepare(dialog, "cancel.ogg"); dialog.togglePreview(); assert(dialog.previewPlaying());
    dialog.pausePreview(); assert(!dialog.previewPlaying());
    dialog.togglePreview(); dialog.cancel(); assert(!Signals::playing && !std::filesystem::exists("cancel.ogg"));
    prepare(dialog, "send.ogg"); dialog.togglePreview(); dialog.releaseFile();
    assert(!dialog.active() && !Signals::playing && std::filesystem::exists("send.ogg"));
    // A second preview needs a fresh player after closing the first dialog.
    prepare(dialog, "second.ogg"); dialog.togglePreview(); assert(dialog.previewPlaying());
    dialog.open("replacement.ogg"); assert(!std::filesystem::exists("second.ogg") && !Signals::playing);
    Signals::fail_start = true; dialog.record(); assert(dialog.state() == State::Error);
    Signals::fail_start = false; Signals::failed_capture = true;
    dialog.open("failure.ogg"); dialog.record(); Signals::running = false; dialog.pump(200000);
    assert(dialog.state() == State::Error && !Signals::encoding && dialog.path().empty());
    Signals::failed_capture = false;
    dialog.cancel(); std::remove("send.ogg");
    std::puts("Audio dialog: explicit capture/review/send, failure, real levels, repeat preview and file ownership: PASS");
}
