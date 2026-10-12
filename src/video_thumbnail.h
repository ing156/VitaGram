#pragma once
#include "image_decoder.h"
#include <algorithm>
#include <cstring>
#include <climits>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

// Decode one thumbnail frame on the image worker. Input is already read through
// vault::Reader, so both plain and encrypted downloads stay in RAM here.
namespace video_thumbnail {
constexpr int max_side = 1920;
constexpr int max_pixels = 1280 * 720;
struct Input {
    const std::vector<uint8_t>& bytes;
    const std::function<bool()>& proceed;
    int64_t position = 0;
    static int read(void* opaque, uint8_t* buffer, int count) {
        auto& self = *static_cast<Input*>(opaque);
        if (!self.proceed()) return AVERROR_EXIT;
        count = int(std::min<int64_t>(count, self.bytes.size() - self.position));
        if (count <= 0) return AVERROR_EOF;
        std::memcpy(buffer, self.bytes.data() + self.position, count);
        self.position += count; return count;
    }
    static int64_t seek(void* opaque, int64_t offset, int whence) {
        auto& self = *static_cast<Input*>(opaque);
        if (whence & AVSEEK_SIZE) return self.bytes.size();
        whence &= ~AVSEEK_FORCE;
        int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? self.position :
            whence == SEEK_END ? int64_t(self.bytes.size()) : -1;
        if (base < 0 || offset < -base || offset > int64_t(self.bytes.size()) - base) return AVERROR(EINVAL);
        return self.position = base + offset;
    }
    static int interrupted(void* opaque) { return !static_cast<Input*>(opaque)->proceed(); }
};
struct Decoder {
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* scale = nullptr;
    ~Decoder() {
        sws_freeContext(scale); av_frame_free(&frame); av_packet_free(&packet);
        avcodec_free_context(&codec); avformat_close_input(&format);
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    }
};
inline DecodedImage decode(const std::vector<uint8_t>& bytes, const std::function<bool()>& proceed) {
    if (bytes.empty() || !proceed()) return {};
    Input input{bytes, proceed}; Decoder d;
    auto* buffer = static_cast<uint8_t*>(av_malloc(32768));
    if (!buffer) return {};
    d.io = avio_alloc_context(buffer, 32768, 0, &input, Input::read, nullptr, Input::seek);
    if (!d.io) { av_free(buffer); return {}; }
    d.format = avformat_alloc_context();
    if (!d.format) return {};
    d.format->pb = d.io; d.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    d.format->interrupt_callback = {Input::interrupted, &input};
    d.format->probesize = 256 * 1024;
    d.format->max_analyze_duration = 1000000;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "format_whitelist", "mov,gif", 0);
    const int opened = avformat_open_input(&d.format, nullptr, nullptr, &options);
    av_dict_free(&options);
    if (!proceed()) return {};
    if (opened < 0) return image_failure("video_demux");
    // MP4/GIF headers describe the stream; avoid scanning for stream information.
    int stream = -1;
    for (unsigned i = 0; i < d.format->nb_streams; ++i)
        if (d.format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { stream = int(i); break; }
    if (stream < 0) return image_failure("video_stream");
    const auto* params = d.format->streams[stream]->codecpar;
    if (params->width <= 0 || params->height <= 0 || params->width > max_side || params->height > max_side ||
        int64_t(params->width) * params->height > max_pixels ||
        (params->codec_id != AV_CODEC_ID_H264 && params->codec_id != AV_CODEC_ID_GIF)) return image_failure("video_format_limits");
    // Software H.264 avoids competing with the playback hardware decoder.
    const AVCodec* decoder = avcodec_find_decoder_by_name(params->codec_id == AV_CODEC_ID_H264 ? "h264" : "gif");
    d.codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!d.codec || avcodec_parameters_to_context(d.codec, params) < 0) return image_failure("video_codec");
    d.codec->thread_count = 1; d.codec->max_pixels = max_pixels;
    if (avcodec_open2(d.codec, decoder, nullptr) < 0) return image_failure("video_codec_open");
    d.packet = av_packet_alloc(); d.frame = av_frame_alloc();
    if (!d.packet || !d.frame) return {};
    for (unsigned packets = 0; packets < 64 && proceed(); ++packets) {
        const int read = av_read_frame(d.format, d.packet);
        if (read >= 0 && d.packet->stream_index != stream) { av_packet_unref(d.packet); continue; }
        const int sent = avcodec_send_packet(d.codec, read < 0 ? nullptr : d.packet);
        av_packet_unref(d.packet);
        if (sent < 0) return image_failure("video_packet");
        if (avcodec_receive_frame(d.codec, d.frame) == 0) {
            if (!proceed() || d.frame->width <= 0 || d.frame->height <= 0 ||
                d.frame->width > max_side || d.frame->height > max_side ||
                int64_t(d.frame->width) * d.frame->height > max_pixels) return {};
            DecodedImage out;
            const unsigned longest = std::max(d.frame->width, d.frame->height);
            out.width = std::max(1u, unsigned(d.frame->width) * std::min(longest, 320u) / longest);
            out.height = std::max(1u, unsigned(d.frame->height) * std::min(longest, 320u) / longest);
            out.rgba.resize(size_t(out.width) * out.height * 4);
            d.scale = sws_getContext(d.frame->width, d.frame->height, AVPixelFormat(d.frame->format),
                out.width, out.height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!d.scale) return image_failure("video_scale");
            uint8_t* target[] = {out.rgba.data(), nullptr, nullptr, nullptr};
            int strides[] = {int(out.width * 4), 0, 0, 0};
            const int scaled = sws_scale(d.scale, d.frame->data, d.frame->linesize, 0, d.frame->height, target, strides);
            if (!proceed()) return {};
            if (scaled != int(out.height)) return image_failure("video_scale");
            return out;
        }
        if (read < 0) break;
    }
    return proceed() ? image_failure("video_no_frame") : DecodedImage{};
}
}
