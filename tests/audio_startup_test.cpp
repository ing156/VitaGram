#include "download_avio.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

enum class Input { File, Download, Prefix };
struct Audio { int64_t samples = 0; uint64_t pcm_hash = 0; std::vector<int16_t> first_block; };

// Exercise decoding/resampling without stream analysis, including MP3's initially unknown rate/layout.
static Audio decode(const char* path, Input input, int64_t seek_us = 0, bool analyze = false) {
    auto* format = avformat_alloc_context(); assert(format);
    format->probesize = 256 * 1024;
    format->max_analyze_duration = 2 * AV_TIME_BASE;
    auto started = std::chrono::steady_clock::now();
    format->interrupt_callback = {+[](void* opaque) -> int {
        return std::chrono::steady_clock::now() - *static_cast<const std::chrono::steady_clock::time_point*>(opaque) > std::chrono::seconds(5);
    }, &started};
    std::ifstream file(path, std::ios::binary | std::ios::ate); assert(file);
    const int64_t size = file.tellg(); assert(size > 256 * 1024);
    auto state = std::make_shared<download_stream::State>(1);
    state->update(path, size, 0, input == Input::Prefix ? 256 * 1024 : size, input != Input::Prefix);
    DownloadAvio io;
    if (input != Input::File) assert(io.attach(format, state) == 1);
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist", "mp3,aac,mov,mp4,m4a,3gp,3g2,mj2,ogg,flac,wav", 0);
    assert(avformat_open_input(&format, path, nullptr, &options) >= 0);
    av_dict_free(&options);
    int index = -1;
    for (unsigned i = 0; i < format->nb_streams; ++i)
        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { index = int(i); break; }
    assert(index >= 0);
    for (unsigned i = 0; i < format->nb_streams; ++i)
        if (int(i) != index) format->streams[i]->discard = AVDISCARD_ALL;
    if (analyze) assert(avformat_find_stream_info(format, nullptr) >= 0);
    const auto* params = format->streams[index]->codecpar;
    auto* codec = avcodec_alloc_context3(avcodec_find_decoder(params->codec_id)); assert(codec);
    assert(avcodec_parameters_to_context(codec, params) >= 0);
    codec->thread_count = 1;
    codec->pkt_timebase = format->streams[index]->time_base;
    assert(avcodec_open2(codec, codec->codec, nullptr) >= 0);
    if (seek_us) {
        const int64_t origin = format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
        assert(av_seek_frame(format, index, av_rescale_q(seek_us + origin, AV_TIME_BASE_Q, codec->pkt_timebase), AVSEEK_FLAG_BACKWARD) >= 0);
    }
    auto* packet = av_packet_alloc(); auto* frame = av_frame_alloc(); assert(packet && frame);
    SwrContext* resampler = nullptr;
    Audio audio;
    auto output = [&](const int16_t* data, int count) {
        assert(count >= 0);
        audio.samples += count;
        for (int i = 0; i < count * 2; ++i) audio.pcm_hash = audio.pcm_hash * 1099511628211ULL ^ uint16_t(data[i]);
        const size_t keep = std::min<size_t>(size_t(count) * 2, 2048 - audio.first_block.size());
        audio.first_block.insert(audio.first_block.end(), data, data + keep);
    };
    auto drain = [&] {
        int rc;
        while ((rc = avcodec_receive_frame(codec, frame)) >= 0) {
            assert(frame->sample_rate > 0 && frame->ch_layout.nb_channels > 0);
            if (!resampler) {
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                assert(swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, 48000,
                    &frame->ch_layout, AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr) >= 0);
                assert(swr_init(resampler) >= 0);
            }
            const int capacity = swr_get_out_samples(resampler, frame->nb_samples);
            assert(capacity > 0 && capacity <= 48000);
            std::vector<int16_t> pcm(capacity * 2);
            uint8_t* dest = reinterpret_cast<uint8_t*>(pcm.data());
            output(pcm.data(), swr_convert(resampler, &dest, capacity, (const uint8_t**)frame->extended_data, frame->nb_samples));
            av_frame_unref(frame);
        }
        assert(rc == AVERROR(EAGAIN) || rc == AVERROR_EOF);
    };
    int rc = 0;
    while ((input != Input::Prefix || audio.samples < 1024) && (rc = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == index) { assert(avcodec_send_packet(codec, packet) >= 0); drain(); }
        av_packet_unref(packet);
    }
    if (input != Input::Prefix) {
        assert(rc == AVERROR_EOF && avcodec_send_packet(codec, nullptr) >= 0); drain();
        assert(resampler);
        int16_t tail[8192]; uint8_t* dest = reinterpret_cast<uint8_t*>(tail);
        int count;
        while ((count = swr_convert(resampler, &dest, 4096, nullptr, 0)) > 0) output(tail, count);
        assert(count == 0);
    } else assert(audio.samples >= 1024 && !state->snapshot().complete);
    assert(audio.first_block.size() == 2048);
    av_packet_free(&packet); av_frame_free(&frame); swr_free(&resampler);
    avcodec_free_context(&codec); avformat_close_input(&format);
    return audio;
}

int main(int argc, char** argv) {
    assert(argc > 1);
    av_log_set_level(AV_LOG_ERROR);
    for (int i = 1; i < argc; ++i) {
        const auto baseline = decode(argv[i], Input::File, 0, true);
        const auto plain = decode(argv[i], Input::File);
        const auto complete = decode(argv[i], Input::Download);
        const auto prefix = decode(argv[i], Input::Prefix);
        assert(baseline.samples == plain.samples && baseline.pcm_hash == plain.pcm_hash);
        assert(plain.samples == complete.samples && plain.pcm_hash == complete.pcm_hash);
        assert(prefix.first_block == plain.first_block);
        if (plain.samples > 31 * 48000) {
            const auto seek = decode(argv[i], Input::Download, 30000000);
            assert(std::abs(plain.samples - seek.samples - 30 * 48000) < 48000);
        }
        assert(!download_stream::reading());
        std::printf("Audio without stream analysis: %.3f s, file/download PCM match, 256 KiB startup and seek passed: %s\n", plain.samples / 48000.0, argv[i]);
    }
}
