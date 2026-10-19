#pragma once
#include <cerrno>
#include <climits>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/intreadwrite.h>
}

// Native VP8/VP9 decoders leave WebM's BlockAdditional alpha stream to the caller.
class WebmAlpha {
    AVCodecContext* codec_ = nullptr;
    AVFrame* frame_ = nullptr;
public:
    ~WebmAlpha() { av_frame_free(&frame_); avcodec_free_context(&codec_); }
    WebmAlpha() = default;
    WebmAlpha(const WebmAlpha&) = delete;
    WebmAlpha& operator=(const WebmAlpha&) = delete;

    int send(const AVPacket* packet, const AVCodecContext* color) {
        if (color->codec_id != AV_CODEC_ID_VP8 && color->codec_id != AV_CODEC_ID_VP9) return 0;
        size_t size = 0;
        const uint8_t* data = av_packet_get_side_data(packet, AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &size);
        if (!data) return 0;
        if (size < 8) return AVERROR_INVALIDDATA;
        if (AV_RB64(data) != 1) return 0;
        if (size <= 8 || size - 8 > INT_MAX) return AVERROR_INVALIDDATA;
        if (!codec_) {
            const AVCodec* decoder = avcodec_find_decoder(color->codec_id);
            if (!decoder) return AVERROR_DECODER_NOT_FOUND;
            codec_ = avcodec_alloc_context3(decoder);
            frame_ = av_frame_alloc();
            if (!codec_ || !frame_) return AVERROR(ENOMEM);
            codec_->thread_count = 1;
            codec_->max_pixels = color->max_pixels;
            codec_->pkt_timebase = color->pkt_timebase;
            int rc = avcodec_open2(codec_, decoder, nullptr);
            if (rc < 0) return rc;
        }
        AVPacket* alpha = av_packet_alloc();
        if (!alpha) return AVERROR(ENOMEM);
        int rc = av_new_packet(alpha, static_cast<int>(size - 8));
        if (rc >= 0) {
            std::memcpy(alpha->data, data + 8, size - 8);
            alpha->pts = packet->pts; alpha->dts = packet->dts;
            rc = avcodec_send_packet(codec_, alpha);
        }
        av_packet_free(&alpha);
        if (rc < 0) return rc;
        av_frame_unref(frame_);
        rc = avcodec_receive_frame(codec_, frame_);
        return rc == AVERROR(EAGAIN) ? 0 : rc;
    }

    int attach(AVFrame* color) {
        if (!codec_) return 0;
        if (!frame_->data[0] || frame_->format != AV_PIX_FMT_YUV420P ||
            color->format != AV_PIX_FMT_YUV420P || frame_->width != color->width ||
            frame_->height != color->height || frame_->pts != color->pts) return AVERROR_INVALIDDATA;
        AVBufferRef* plane = av_frame_get_plane_buffer(frame_, 0);
        if (!plane) return AVERROR_INVALIDDATA;
        for (auto& buffer : color->buf) {
            if (buffer) continue;
            buffer = av_buffer_ref(plane);
            if (!buffer) return AVERROR(ENOMEM);
            color->data[3] = frame_->data[0];
            color->linesize[3] = frame_->linesize[0];
            color->format = AV_PIX_FMT_YUVA420P;
            return 0;
        }
        return AVERROR_INVALIDDATA;
    }

    void flush() {
        if (codec_) avcodec_flush_buffers(codec_);
        if (frame_) av_frame_unref(frame_);
    }
};
