#pragma once
#include "video_note_frame.h"
#include <array>
#include <cstring>
#include <pthread.h>
#include <vector>

namespace video_note {
// Disk I/O never holds this mutex. A full video queue drops the arriving image;
// audio overflow fails the recording instead of silently shortening its clock.
// The consumer retains its slot until complete(), including during a slow write.
class CaptureBuffer {
public:
    static constexpr unsigned VideoSlots=64, AudioSlots=128, AudioGrain=512;
    static constexpr size_t VideoStdioBytes=256*1024, AudioStdioBytes=32*1024;
    enum class Result { Queued, Full, Stopped };
    struct Item { bool audio; uint32_t pts; const void* data; size_t bytes; };
    struct Stats { unsigned video_peak, audio_peak, dropped; };
    CaptureBuffer() : video_(VideoSlots), audio_(AudioSlots) {}
    ~CaptureBuffer() { pthread_cond_destroy(&ready_); pthread_mutex_destroy(&mutex_); }
    CaptureBuffer(const CaptureBuffer&)=delete;
    CaptureBuffer& operator=(const CaptureBuffer&)=delete;
    Result video(uint32_t pts, const uint8_t* pixels) {
        pthread_mutex_lock(&mutex_);
        Result result=Result::Stopped;
        if (!stopped_ && !finished_) {
            if (video_count_==VideoSlots) { ++dropped_; result=Result::Full; }
            else {
                auto& slot=video_[(video_head_+video_count_)%VideoSlots];
                slot.pts=pts; std::memcpy(slot.pixels.data(),pixels,FrameBytes);
                ++video_count_; video_peak_=std::max(video_peak_,video_count_);
                result=Result::Queued; pthread_cond_signal(&ready_);
            }
        }
        pthread_mutex_unlock(&mutex_); return result;
    }
    Result audio(const int16_t* samples, unsigned count) {
        pthread_mutex_lock(&mutex_);
        Result result=Result::Stopped;
        if (!stopped_ && !finished_) {
            if (audio_count_==AudioSlots || count>AudioGrain) result=Result::Full;
            else {
                auto& slot=audio_[(audio_head_+audio_count_)%AudioSlots];
                slot.count=count; std::memcpy(slot.samples.data(),samples,count*sizeof(int16_t));
                ++audio_count_; audio_peak_=std::max(audio_peak_,audio_count_);
                result=Result::Queued; pthread_cond_signal(&ready_);
            }
        }
        pthread_mutex_unlock(&mutex_); return result;
    }
    bool next(Item& item) {
        pthread_mutex_lock(&mutex_);
        while (!stopped_ && !finished_ && !video_count_ && !audio_count_)
            pthread_cond_wait(&ready_,&mutex_);
        bool available=!stopped_ && (video_count_ || audio_count_);
        if (available) {
            if (audio_count_) {
                const auto& slot=audio_[audio_head_];
                item={true,0,slot.samples.data(),slot.count*sizeof(int16_t)};
            } else {
                const auto& slot=video_[video_head_];
                item={false,slot.pts,slot.pixels.data(),FrameBytes};
            }
        }
        pthread_mutex_unlock(&mutex_); return available;
    }
    void complete(const Item& item) {
        pthread_mutex_lock(&mutex_);
        if (item.audio) { audio_head_=(audio_head_+1)%AudioSlots; --audio_count_; }
        else { video_head_=(video_head_+1)%VideoSlots; --video_count_; }
        pthread_mutex_unlock(&mutex_);
    }
    void finish() { stop(false); } // Drain after both producers have stopped.
    void abort() { stop(true); } // Discard queued data; wake a waiting writer.
    Stats stats() {
        pthread_mutex_lock(&mutex_); Stats value{video_peak_,audio_peak_,dropped_};
        pthread_mutex_unlock(&mutex_); return value;
    }
    std::array<char,VideoStdioBytes> video_stdio{};
    std::array<char,AudioStdioBytes> audio_stdio{};
private:
    struct Video { uint32_t pts; std::array<uint8_t,FrameBytes> pixels; };
    struct Audio { unsigned count; std::array<int16_t,AudioGrain> samples; };
    void stop(bool abort) {
        pthread_mutex_lock(&mutex_);
        finished_=true; stopped_=stopped_ || abort; pthread_cond_broadcast(&ready_);
        pthread_mutex_unlock(&mutex_);
    }
    pthread_mutex_t mutex_=PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t ready_=PTHREAD_COND_INITIALIZER;
    std::vector<Video> video_;
    std::vector<Audio> audio_;
    unsigned video_head_=0, audio_head_=0, video_count_=0, audio_count_=0;
    unsigned video_peak_=0, audio_peak_=0, dropped_=0;
    bool finished_=false, stopped_=false;
};
}
