#include "audio_recorder.h"
#include "cpu_scheduler.h"
#include <psp2/audioin.h>
#include <psp2/kernel/threadmgr.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <sys/stat.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace {
constexpr uint32_t sample_rate = 16000;
constexpr uint16_t channels = 1;
constexpr uint16_t bits = 16;
constexpr int grain = 512;

void put16(unsigned char* p, uint16_t value) {
    p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8);
}
void put32(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16); p[3] = (unsigned char)(value >> 24);
}
}

AudioRecorder::~AudioRecorder() {
    cancel();
    pthread_mutex_destroy(&mutex_);
}

bool AudioRecorder::start(const std::string& path) {
    if (recording_.load() || path.empty()) return false;
    pcm_path_ = path + ".pcm.wav";
    FILE* file = std::fopen(pcm_path_.c_str(), "wb");
    if (!file) { status_ = "Не удалось создать файл записи"; return false; }
    unsigned char empty[44]{}; std::fwrite(empty, 1, sizeof(empty), file); std::fclose(file);
    path_ = path; data_bytes_ = 0; stop_ = false; level_ = 0; peak_ = 0; status_ = "Запись...";
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 64 * 1024);
    int result = pthread_create(&thread_, &attr, entry, this); pthread_attr_destroy(&attr);
    if (result != 0) { status_ = "Не удалось запустить запись"; return false; }
    started_ = true; recording_ = true; return true;
}

void AudioRecorder::cancel() {
    if (!started_) return;
    stop_ = true;
    pthread_join(thread_, nullptr);
    started_ = false; recording_ = false;
    std::remove(pcm_path_.c_str()); std::remove(path_.c_str());
}

std::string AudioRecorder::stop() {
    if (!started_) return {};
    stop_ = true;
    pthread_join(thread_, nullptr);
    started_ = false; recording_ = false;
    if (data_bytes_ == 0 || !encodeOggOpus()) { std::remove(pcm_path_.c_str()); std::remove(path_.c_str()); status_ = "Не удалось кодировать Ogg/Opus"; return {}; }
    std::remove(pcm_path_.c_str()); status_ = "Запись готова"; return path_;
}

bool AudioRecorder::encodeOggOpus() {
    FILE* input = std::fopen(pcm_path_.c_str(), "rb");
    if (!input) return false;
    std::fseek(input, 44, SEEK_SET);
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    AVCodecContext* context = codec ? avcodec_alloc_context3(codec) : nullptr;
    AVFormatContext* format = nullptr;
    AVStream* stream = nullptr;
    SwrContext* resampler = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    bool ok = false;
    do {
        if (!context || !codec) break;
        context->bit_rate = 24000;
        context->sample_rate = sample_rate;
        av_channel_layout_default(&context->ch_layout, channels);
        context->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLT;
        context->time_base = AVRational{1, (int)sample_rate};
        if (avcodec_open2(context, codec, nullptr) < 0) break;
        if (avformat_alloc_output_context2(&format, nullptr, "ogg", path_.c_str()) < 0 || !format) break;
        stream = avformat_new_stream(format, nullptr);
        if (!stream || avcodec_parameters_from_context(stream->codecpar, context) < 0) break;
        stream->time_base = context->time_base;
        if (!(format->oformat->flags & AVFMT_NOFILE) && avio_open(&format->pb, path_.c_str(), AVIO_FLAG_WRITE) < 0) break;
        if (avformat_write_header(format, nullptr) < 0) break;
        if (swr_alloc_set_opts2(&resampler, &context->ch_layout, context->sample_fmt, sample_rate,
            &context->ch_layout, AV_SAMPLE_FMT_S16, sample_rate, 0, nullptr) < 0)
            break;
        if (!resampler || swr_init(resampler) < 0) break;
        frame = av_frame_alloc(); packet = av_packet_alloc();
        if (!frame || !packet) break;
        frame->format = context->sample_fmt; frame->sample_rate = sample_rate; frame->ch_layout = context->ch_layout;
        const int frame_samples = context->frame_size > 0 ? context->frame_size : 320;
        frame->nb_samples = frame_samples;
        if (av_frame_get_buffer(frame, 0) < 0) break;
        std::vector<int16_t> pcm(frame_samples * channels);
        while (true) {
            size_t count = std::fread(pcm.data(), sizeof(int16_t), pcm.size(), input);
            if (!count) break;
            if (count < pcm.size()) std::fill(pcm.begin() + count, pcm.end(), 0);
            const uint8_t* source[] = {reinterpret_cast<const uint8_t*>(pcm.data())};
            if (swr_convert(resampler, frame->data, frame_samples, source, frame_samples) < 0) break;
                frame->pts = av_rescale_q((int64_t)(std::ftell(input) - 44) / 2,
                    AVRational{1, (int)sample_rate}, context->time_base);
            if (avcodec_send_frame(context, frame) < 0) break;
            while (avcodec_receive_packet(context, packet) >= 0) {
                av_packet_rescale_ts(packet, context->time_base, stream->time_base);
                packet->stream_index = stream->index;
                if (av_interleaved_write_frame(format, packet) < 0) break;
                av_packet_unref(packet);
            }
            if (count < pcm.size()) break;
        }
        avcodec_send_frame(context, nullptr);
        while (avcodec_receive_packet(context, packet) >= 0) {
            av_packet_rescale_ts(packet, context->time_base, stream->time_base); packet->stream_index = stream->index;
            if (av_interleaved_write_frame(format, packet) < 0) break;
            av_packet_unref(packet);
        }
        av_write_trailer(format); ok = true;
    } while (false);
    if (format && !(format->oformat->flags & AVFMT_NOFILE)) avio_closep(&format->pb);
    if (format) avformat_free_context(format);
    av_frame_free(&frame); av_packet_free(&packet); swr_free(&resampler); avcodec_free_context(&context); std::fclose(input);
    if (!ok) std::remove(path_.c_str());
    return ok;
}

int AudioRecorder::seconds() const {
    return (int)(data_bytes_ / (sample_rate * channels * (bits / 8)));
}

std::string AudioRecorder::status() const {
    pthread_mutex_lock(&mutex_); auto result = status_; pthread_mutex_unlock(&mutex_); return result;
}

void* AudioRecorder::entry(void* self) { static_cast<AudioRecorder*>(self)->run(); return nullptr; }

void AudioRecorder::run() {
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Audio);
    int port = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, grain, sample_rate, SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (port < 0) { status_ = "Микрофон недоступен"; return; }
    FILE* file = std::fopen(pcm_path_.c_str(), "ab");
    if (!file) { sceAudioInReleasePort(port); status_ = "Ошибка файла записи"; return; }
    int16_t samples[grain]{};
    while (!stop_) {
        int result = sceAudioInInput(port, samples);
        if (result < 0) { status_ = "Ошибка чтения микрофона"; break; }
        const int count = result > 0 && result <= grain ? result : grain;
        int maximum = 0;
        uint64_t energy = 0;
        for (int i = 0; i < count; ++i) {
            int value = samples[i] < 0 ? -samples[i] : samples[i];
            maximum = std::max(maximum, value);
            energy += (uint64_t)value * value;
        }
        const int average = count ? (int)std::sqrt((double)energy / count) : 0;
        level_ = std::min(100, average * 100 / 32768);
        peak_ = std::min(100, maximum * 100 / 32768);
        size_t written = std::fwrite(samples, sizeof(samples[0]), count, file);
        data_bytes_ += (uint32_t)(written * sizeof(samples[0]));
    }
    std::fflush(file); std::fclose(file); sceAudioInReleasePort(port);
}
