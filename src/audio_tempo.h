#pragma once
#include <cstring>
#include <cstdio>
#include <cstdint>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/channel_layout.h>
}

// The player feeds 48 kHz stereo PCM; atempo changes duration without changing pitch.
class AudioTempo {
public:
    ~AudioTempo() { av_frame_free(&frame_); avfilter_graph_free(&graph_); }
    int open(int percent) {
        graph_ = avfilter_graph_alloc();
        frame_ = av_frame_alloc();
        if (!graph_ || !frame_) return AVERROR(ENOMEM);
        graph_->nb_threads = 1;
        int rc = avfilter_graph_create_filter(&input_, avfilter_get_by_name("abuffer"), "input",
            "time_base=1/48000:sample_rate=48000:sample_fmt=s16:channel_layout=stereo", nullptr, graph_);
        AVFilterContext* tempo = nullptr;
        char rate[32]; std::snprintf(rate, sizeof(rate), "tempo=%.2f", percent / 100.0);
        if (rc >= 0) rc = avfilter_graph_create_filter(&tempo, avfilter_get_by_name("atempo"), "tempo", rate, nullptr, graph_);
        if (rc >= 0) rc = avfilter_graph_create_filter(&output_, avfilter_get_by_name("abuffersink"), "output", nullptr, nullptr, graph_);
        if (rc >= 0) rc = avfilter_link(input_, 0, tempo, 0);
        if (rc >= 0) rc = avfilter_link(tempo, 0, output_, 0);
        if (rc >= 0) rc = avfilter_graph_config(graph_, nullptr);
        return rc;
    }
    template<class Output> int push(const int16_t* pcm, int frames, Output output) {
        av_frame_unref(frame_);
        int rc = 0;
        if (pcm) {
            frame_->format = AV_SAMPLE_FMT_S16; frame_->sample_rate = 48000;
            frame_->ch_layout = AV_CHANNEL_LAYOUT_STEREO; frame_->nb_samples = frames;
            frame_->pts = samples_;
            rc = av_frame_get_buffer(frame_, 0);
            if (rc < 0) return rc;
            std::memcpy(frame_->data[0], pcm, frames * 2 * sizeof(int16_t));
            samples_ += frames;
        }
        rc = av_buffersrc_add_frame_flags(input_, pcm ? frame_ : nullptr, 0);
        if (rc < 0) return rc;
        while ((rc = av_buffersink_get_frame(output_, frame_)) >= 0) {
            const bool ok = output(reinterpret_cast<const int16_t*>(frame_->data[0]), frame_->nb_samples);
            av_frame_unref(frame_);
            if (!ok) return AVERROR_EXIT;
        }
        return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF ? 0 : rc;
    }
private:
    AVFilterGraph* graph_ = nullptr;
    AVFilterContext *input_ = nullptr, *output_ = nullptr;
    AVFrame* frame_ = nullptr;
    int64_t samples_ = 0;
};
