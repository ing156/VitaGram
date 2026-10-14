#pragma once
#include "i18n.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <string>
#include <memory>

// Capture owns temporary files until takeCompleted(); the dialog owns the review
// until an explicit successful send transfers it to Telegram's upload tracking.
template<class Capture, class Playback>
class AudioRecordingDialog {
public:
    enum class State { Closed, Preview, Recording, Encoding, Review, Error };
    ~AudioRecordingDialog() { cancel(); }
    void open(const std::string& path) {
        cancel(); capture_path_ = path; state_ = State::Preview;
        levels_.fill(0); sample_at_ = 0;
    }
    void record() {
        if (state_ != State::Preview) return;
        state_ = capture_.start(capture_path_) ? State::Recording : State::Error;
    }
    void finish() {
        if (state_ != State::Recording) return;
        capture_.stop();
        state_ = capture_.processing() ? State::Encoding : State::Error;
    }
    void pump(int64_t now) {
        if (state_ == State::Recording) {
            if (!capture_.recording()) { finish(); return; }
            if (!sample_at_ || now - sample_at_ >= 50000) {
                std::move(levels_.begin()+1, levels_.end(), levels_.begin());
                levels_.back() = std::clamp(std::max(capture_.level()*3, capture_.peak()*2), 0, 100);
                sample_at_ = now;
            }
        } else if (state_ == State::Encoding) {
            const std::string ready = capture_.takeCompleted();
            if (!ready.empty()) { review_path_ = ready; state_ = State::Review; }
            else if (!capture_.processing()) state_ = State::Error;
        }
    }
    void togglePreview() {
        if (state_ != State::Review) return;
        if (!playback_) playback_ = std::make_unique<Playback>();
        playback_->setSource(review_path_);
        playback_->toggle();
    }
    void pausePreview() { if (playback_) playback_->setPlaying(false); }
    bool previewPlaying() const { return playback_ && playback_->playing(); }
    double previewSeconds() const { return playback_ ? playback_->positionSeconds() : 0; }
    i18n::Text previewError() { return playback_ ? i18n::Text(playback_->error()) : i18n::Text(); }
    void releaseFile() {
        if (state_ != State::Review) return;
        playback_.reset();
        review_path_.clear(); // Upload now owns the file.
        capture_.cancel(); capture_path_.clear(); state_ = State::Closed;
    }
    void cancel() {
        playback_.reset(); capture_.cancel();
        if (!review_path_.empty()) std::remove(review_path_.c_str());
        review_path_.clear(); capture_path_.clear(); state_ = State::Closed;
    }
    State state() const { return state_; }
    bool active() const { return state_ != State::Closed; }
    bool recording() const { return state_ == State::Recording; }
    bool processing() const { return state_ == State::Encoding; }
    int seconds() const { return state_ == State::Preview ? 0 : capture_.seconds(); }
    int conversionPercent() const { return capture_.conversionPercent(); }
    const std::string& path() const { return review_path_; }
    const std::array<int,64>& levels() const { return levels_; }
    void setStatus(const i18n::Text& text) { capture_.setStatus(text); }
    i18n::Text status() const {
        if (state_ == State::Preview) return i18n::text("audio_record.preview");
        if (state_ == State::Review) return i18n::text("audio_record.review");
        if (state_ == State::Encoding) return i18n::text("rec.converting", {std::to_string(capture_.conversionPercent())});
        return capture_.status();
    }
private:
    Capture capture_;
    std::unique_ptr<Playback> playback_;
    State state_ = State::Closed;
    std::string capture_path_, review_path_;
    std::array<int,64> levels_{};
    int64_t sample_at_ = 0;
};
