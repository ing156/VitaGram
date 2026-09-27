#include "audio_recorder.h"
#include "config.h"
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
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

namespace {
constexpr uint32_t sample_rate = 16000;
constexpr uint32_t opus_sample_rate = 48000;
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
    // The encoder still uses path_, pcm_path_ and data_bytes_. Keep them intact
    // until takeCompleted() has handed the finished recording to the caller.
    if (started_ && !recording_) { pthread_join(thread_, nullptr); started_ = false; std::remove(pcm_path_.c_str()); }
    if (started_ || encode_started_ || processing_.load() || path.empty()) return false;
    std::string file_name = path;
    const size_t slash = file_name.find_last_of("/\\");
    if (slash != std::string::npos) file_name = file_name.substr(slash + 1);
    if (file_name.empty()) file_name = "voice-message.ogg";
    path_ = std::string(VITAGRAM_FILES_DIR) + "/" + file_name;
    pcm_path_ = path_ + ".pcm.wav";
    FILE* file = std::fopen(pcm_path_.c_str(), "wb");
    if (!file) { status_ = "Не удалось создать файл записи"; return false; }
    unsigned char empty[44]{}; std::fwrite(empty, 1, sizeof(empty), file); std::fclose(file);
    data_bytes_ = 0; capture_failed_ = false; stop_ = false; level_ = 0; peak_ = 0; conversion_percent_ = 0; completed_path_.clear(); status_ = "Запись...";
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 64 * 1024);
    recording_ = true;
    int result = pthread_create(&thread_, &attr, entry, this); pthread_attr_destroy(&attr);
    if (result != 0) { recording_ = false; std::remove(pcm_path_.c_str()); status_ = "Не удалось запустить запись"; return false; }
    started_ = true; return true;
}

void AudioRecorder::cancel() {
    cancel_encode_ = true;
    if (started_) {
        stop_ = true;
        pthread_join(thread_, nullptr);
        started_ = false; recording_ = false;
    }
    if (encode_started_) {
        pthread_join(encode_thread_, nullptr);
        encode_started_ = false;
        processing_ = false;
    }
    std::remove(pcm_path_.c_str()); std::remove(path_.c_str());
    pcm_path_.clear(); path_.clear();
    pthread_mutex_lock(&mutex_);
    completed_path_.clear();
    pthread_mutex_unlock(&mutex_);
}

std::string AudioRecorder::stop() {
    if (!started_) return {};
    stop_ = true;
    pthread_join(thread_, nullptr);
    started_ = false; recording_ = false;
    if (capture_failed_) { std::remove(pcm_path_.c_str()); return {}; }
    if (data_bytes_ == 0) { std::remove(pcm_path_.c_str()); status_ = "Пустая запись"; return {}; }
    cancel_encode_ = false;
    conversion_percent_ = 0; processing_ = true; status_ = "Конвертация Ogg/Opus: 0%";
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 512 * 1024);
    if (pthread_create(&encode_thread_, &attr, encodeEntry, this) != 0) {
        pthread_attr_destroy(&attr); processing_ = false; std::remove(pcm_path_.c_str()); status_ = "Не удалось запустить конвертацию"; return {};
    }
    pthread_attr_destroy(&attr);
    encode_started_ = true;
    return {};
}

std::string AudioRecorder::takeCompleted() {
    if (!encode_started_ || processing_.load()) return {};
    // A finished pthread remains joinable even after processing_ becomes false.
    pthread_join(encode_thread_, nullptr);
    encode_started_ = false;
    pthread_mutex_lock(&mutex_);
    std::string result = completed_path_;
    completed_path_.clear();
    pthread_mutex_unlock(&mutex_);
    // Telegram uploads asynchronously; cancel/destruction must not delete a
    // recording whose ownership has already passed to the upload caller.
    if (!result.empty()) path_.clear();
    pcm_path_.clear();
    return result;
}

void AudioRecorder::runEncode() {
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Video);
    std::string error;
    bool ok = encodeOggOpus(error);
    std::remove(pcm_path_.c_str());
    pthread_mutex_lock(&mutex_);
    if (ok) { completed_path_ = path_; status_ = "Ogg/Opus готов, отправка..."; }
    else { std::remove(path_.c_str()); status_ = "Ошибка Ogg/Opus: " + error; }
    pthread_mutex_unlock(&mutex_);
    processing_ = false;
}

bool AudioRecorder::encodeOggOpus(std::string& error) {
    FILE* input = std::fopen(pcm_path_.c_str(), "rb");
    if (!input) { error = "не открыт PCM-файл"; return false; }
    std::fseek(input, 44, SEEK_SET);
    struct stat input_info{};
    const long total_bytes = !stat(pcm_path_.c_str(), &input_info) ? std::max<long>(1, input_info.st_size - 44) : 1;
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    AVCodecContext* context = codec ? avcodec_alloc_context3(codec) : nullptr;
    AVFormatContext* format = nullptr;
    AVStream* stream = nullptr;
    SwrContext* resampler = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    bool ok = false;
    do {
        if (!context || !codec) { error = "Opus encoder недоступен"; break; }
        context->bit_rate = 24000;
        context->sample_rate = opus_sample_rate;
        av_channel_layout_default(&context->ch_layout, channels);
        context->sample_fmt = AV_SAMPLE_FMT_FLTP;
        context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        context->time_base = AVRational{1, (int)opus_sample_rate};
        int open_result = avcodec_open2(context, codec, nullptr);
        if (open_result < 0) {
            char message[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(open_result, message, sizeof(message));
            error = std::string("Opus encoder: ") + message;
            break;
        }
        if (avformat_alloc_output_context2(&format, nullptr, "ogg", path_.c_str()) < 0 || !format) { error = "Ogg muxer недоступен"; break; }
        stream = avformat_new_stream(format, nullptr);
        if (!stream || avcodec_parameters_from_context(stream->codecpar, context) < 0) { error = "не создан Ogg stream"; break; }
        stream->time_base = context->time_base;
        if (!(format->oformat->flags & AVFMT_NOFILE)) {
            const std::string file_url = "file:" + path_;
            int open_result = avio_open(&format->pb, file_url.c_str(), AVIO_FLAG_WRITE);
            if (open_result < 0) {
                char message[AV_ERROR_MAX_STRING_SIZE]{};
                av_strerror(open_result, message, sizeof(message));
                error = std::string("не открыт Ogg-файл: ") + message;
                break;
            }
        }
        if (avformat_write_header(format, nullptr) < 0) { error = "ошибка заголовка Ogg"; break; }
        if (swr_alloc_set_opts2(&resampler, &context->ch_layout, context->sample_fmt, opus_sample_rate,
            &context->ch_layout, AV_SAMPLE_FMT_S16, sample_rate, 0, nullptr) < 0)
            { error = "не создан resampler"; break; }
        if (!resampler || swr_init(resampler) < 0) { error = "ошибка PCM resampler"; break; }
        frame = av_frame_alloc(); packet = av_packet_alloc();
        if (!frame || !packet) { error = "не выделена память encoder"; break; }
        frame->format = context->sample_fmt; frame->sample_rate = opus_sample_rate; frame->ch_layout = context->ch_layout;
        const int frame_samples = context->frame_size > 0 ? context->frame_size : 320;
        frame->nb_samples = frame_samples;
        if (av_frame_get_buffer(frame, 0) < 0) { error = "не выделен буфер encoder"; break; }
        const int input_samples = std::max(1, frame_samples * (int)sample_rate / (int)opus_sample_rate);
        std::vector<int16_t> pcm(input_samples * channels);
        int64_t next_pts = 0;
        int packet_count = 0;
        auto failed = [&](int result, const char* operation) {
            if (result >= 0) return false;
            char message[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(result, message, sizeof(message));
            error = std::string(operation) + ": " + message;
            return true;
        };
        auto write_packets = [&]() {
            for (;;) {
                int result = avcodec_receive_packet(context, packet);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
                if (failed(result, "Opus packet")) return false;
                av_packet_rescale_ts(packet, context->time_base, stream->time_base);
                packet->stream_index = stream->index;
                result = av_interleaved_write_frame(format, packet);
                av_packet_unref(packet);
                if (failed(result, "Ogg write")) return false;
                ++packet_count;
            }
        };
        bool eof = false;
        bool drained = false;
        while (!drained && error.empty()) {
            if (cancel_encode_) { error = "конвертация отменена"; break; }
            frame->nb_samples = frame_samples;
            if (failed(av_frame_make_writable(frame), "Opus frame buffer")) break;
            int filled = 0;
            // Resampling has a delay: its first output is shorter than an Opus
            // frame. Accumulate samples; only the very last frame may be short.
            while (filled < frame_samples && !drained) {
                size_t count = eof ? 0 : std::fread(pcm.data(), sizeof(int16_t), pcm.size(), input);
                if (std::ferror(input)) { error = "PCM read failed"; break; }
                if (!count) eof = true;
                const uint8_t* source[] = {reinterpret_cast<const uint8_t*>(pcm.data())};
                uint8_t* output[] = {frame->data[0] + filled * sizeof(float)};
                int converted = swr_convert(resampler, output, frame_samples - filled,
                    eof ? nullptr : source, (int)count);
                if (failed(converted, "PCM resample")) break;
                filled += converted;
                if (eof && converted == 0) drained = true;
            }
            if (!error.empty() || !filled) break;
            frame->nb_samples = filled;
            frame->pts = next_pts;
            next_pts += filled;
            if (failed(avcodec_send_frame(context, frame), "Opus frame") || !write_packets()) break;
            conversion_percent_ = std::min(99, (int)(((std::ftell(input) - 44) * 100) / total_bytes));
        }
        if (!error.empty()) break;
        if (failed(avcodec_send_frame(context, nullptr), "Opus flush") || !write_packets()) break;
        if (!packet_count) { error = "Opus produced no audio packets"; break; }
        if (failed(av_write_trailer(format), "Ogg trailer")) break;
        ok = true;
    } while (false);
    if (format && !(format->oformat->flags & AVFMT_NOFILE)) {
        int close_result = avio_closep(&format->pb);
        if (close_result < 0) { error = "Ogg close failed"; ok = false; }
    }
    if (format) avformat_free_context(format);
    av_frame_free(&frame); av_packet_free(&packet); swr_free(&resampler); avcodec_free_context(&context); std::fclose(input);
    if (!ok) std::remove(path_.c_str());
    conversion_percent_ = ok ? 100 : conversion_percent_.load();
    return ok;
}

int AudioRecorder::seconds() const {
    return (int)(data_bytes_.load() / (sample_rate * channels * (bits / 8)));
}

std::string AudioRecorder::status() const {
    pthread_mutex_lock(&mutex_); auto result = status_; pthread_mutex_unlock(&mutex_); return result;
}

void AudioRecorder::setStatus(const std::string& status) {
    pthread_mutex_lock(&mutex_); status_ = status; pthread_mutex_unlock(&mutex_);
}

void* AudioRecorder::entry(void* self) { static_cast<AudioRecorder*>(self)->run(); return nullptr; }
void* AudioRecorder::encodeEntry(void* self) { static_cast<AudioRecorder*>(self)->runEncode(); return nullptr; }

void AudioRecorder::run() {
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Audio);
    int port = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, grain, sample_rate, SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (port < 0) { setStatus("Микрофон недоступен"); capture_failed_ = true; recording_ = false; return; }
    cpu_scheduler::Media reservation(cpu_scheduler::Role::Audio);
    FILE* file = std::fopen(pcm_path_.c_str(), "ab");
    if (!file) { sceAudioInReleasePort(port); setStatus("Ошибка файла записи"); capture_failed_ = true; recording_ = false; return; }
    int16_t samples[grain]{};
    while (!stop_) {
        int result = sceAudioInInput(port, samples);
        if (result < 0) { setStatus("Ошибка чтения микрофона"); capture_failed_ = true; break; }
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
        if (written != (size_t)count) { setStatus("Нет места для записи аудио"); capture_failed_ = true; break; }
    }
    bool failed = std::fflush(file) != 0;
    failed = std::fclose(file) != 0 || failed;
    if (failed) { setStatus("Ошибка сохранения аудио"); capture_failed_ = true; }
    sceAudioInReleasePort(port);
    recording_ = false;
}
