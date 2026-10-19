#include "diag_log.h"
#include "performance.h"
#include "audio_startup_trace.h"
#include "i18n.h"
#include "voice_player.h"
#include "media_decode_policy.h"
#include "cpu_scheduler.h"
#include "vault.h"
#include "vault_avio.h"
#include "download_avio.h"
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <sys/stat.h>
#include <malloc.h>
#include <cstdlib>
#include <cstdio>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

VoicePlayer::~VoicePlayer() { shutdown(); pthread_mutex_destroy(&mutex_); }
void VoicePlayer::report(const i18n::Text& text) {
    pthread_mutex_lock(&mutex_); status_ = text; pthread_mutex_unlock(&mutex_);
}
i18n::Text VoicePlayer::status() {
    pthread_mutex_lock(&mutex_); auto text = status_; pthread_mutex_unlock(&mutex_); text.refresh();
    if (!text.empty()) return text;
    return i18n::text(finished_ ? "anim.finished_replay" : playing_ ? "voice.playing_pause" : "anim.play_pause");
}
i18n::Text VoicePlayer::error() {
    pthread_mutex_lock(&mutex_); auto text = status_; pthread_mutex_unlock(&mutex_); text.refresh(); return text;
}
void VoicePlayer::setSource(const std::string& path, bool video, bool music, download_stream::Shared stream) {
    performance::SlowScope slow(performance::SlowPhase::Ui,"voiceplayer_setSource");
    pthread_mutex_lock(&mutex_);
    if (path_ == path && video_ == video && music_ == music && stream_ == stream) { pthread_mutex_unlock(&mutex_); return; }
    music_ = music; stream_ = stream;
    video_ = video; clock_ready_ = false; clock_us_ = 0;
    path_ = path; playing_ = false; finished_ = false; samples_ = 0; start_us_ = 0; status_.clear(); ++generation_;
    audio_startup_trace::record(this, generation_, path.empty() ? "source_empty" : "source_ready", 0, 0, stream);
    pthread_mutex_unlock(&mutex_);
    if (!started_ && !path.empty()) {
        audio_startup_trace::record(this, generation_, "worker_create_begin");
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1024 * 1024);
        int rc = pthread_create(&thread_, &attr, entry, this);
        pthread_attr_destroy(&attr);
        audio_startup_trace::record(this, generation_, "worker_create_end", rc);
        if (rc == 0) started_ = true;
        else { report(i18n::text("voice.start_failed")); finished_ = true; }
    }
}
void VoicePlayer::toggle() {
    performance::SlowScope slow(performance::SlowPhase::Ui,"voiceplayer_toggle");
    pthread_mutex_lock(&mutex_);
    if (!path_.empty() && started_) {
        if (finished_) { ++generation_; samples_ = 0; start_us_ = 0; clock_us_ = 0; clock_ready_ = false; finished_ = false; status_.clear(); }
        playing_ = !playing_.load();
        audio_startup_trace::record(this, generation_, playing_ ? "play" : "pause", 0, 0, stream_);
    } else {
        audio_startup_trace::record(this, generation_, "play_ignored_no_source");
    }
    pthread_mutex_unlock(&mutex_);
}
void VoicePlayer::setPlaying(bool playing) {
    if (!finished_) {
        const bool previous = playing_.exchange(playing);
        if (previous != playing) audio_startup_trace::record(this, generation_, playing ? "play_external" : "pause_external");
    }
}
void VoicePlayer::seekTo(int64_t us) {
    performance::SlowScope slow(performance::SlowPhase::Ui,"voiceplayer_seek");
    pthread_mutex_lock(&mutex_);
    if (!path_.empty() && started_) {
        // A new decode generation starts at the target; video audio derives its clock from the first frame's PTS.
        start_us_ = std::max<int64_t>(0, us);
        ++generation_;
        audio_startup_trace::record(this, generation_, "seek", 0, 0, stream_);
        samples_ = video_ ? 0 : start_us_.load() * 48000 / 1000000;
        clock_us_ = 0; clock_ready_ = false; finished_ = false; status_.clear();
    }
    pthread_mutex_unlock(&mutex_);
}
void VoicePlayer::shutdown() {
    performance::SlowScope slow(performance::SlowPhase::Ui,"voiceplayer_shutdown");
    performance::Scope measurement(performance::Metric::PlayerStop);
    stop_ = true;
    if (started_) { pthread_join(thread_, nullptr); started_ = false; }
}
void* VoicePlayer::entry(void* self) { static_cast<VoicePlayer*>(self)->run(); return nullptr; }
void VoicePlayer::run() {
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Audio);
    unsigned seen = 0;
    while (!stop_) {
        pthread_mutex_lock(&mutex_); auto path = path_; unsigned gen = generation_; bool video = video_, music = music_; auto stream = stream_; pthread_mutex_unlock(&mutex_);
        if (!path.empty() && gen != seen && playing_) { seen = gen; decode(path, gen, video, music, stream); }
        else sceKernelDelayThread(10000);
    }
}
void VoicePlayer::decode(const std::string& path, unsigned gen, bool video, bool music, download_stream::Shared stream) {
    const int64_t trace_origin = performance::now();
    auto trace = [&](const char* stage, int rc = 0) { audio_startup_trace::record(this, gen, stage, rc, trace_origin, stream); };
    trace(video ? "decode_video_audio" : music ? "decode_music" : "decode_voice");
    download_stream::Pin reading{bool(stream)};
    cpu_scheduler::Media reservation(cpu_scheduler::Role::Audio);
    struct ProbeJob { VoicePlayer* player; unsigned generation; } probe{this, gen};
    AVFormatContext* format = nullptr;
    VaultAvio sealed_input;
    DownloadAvio growing_input;
    AVCodecContext* codec = nullptr;
    SwrContext* resampler = nullptr;
    // Input the resampler was built for. Decoders (MP3, AAC, FLAC) may change it between frames.
    AVChannelLayout resampler_layout{};
    int resampler_format = -1, resampler_rate = 0;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int port = -1, result = 0;
    i18n::Text error;
    // Video audio uses a bounded producer/consumer ring so decoder and demux
    // bursts cannot delay the next blocking AudioOut submission.
    constexpr int block_frames = 1024;
    constexpr int block_samples = block_frames * 2;
    constexpr unsigned ring_slots = 64;
    constexpr unsigned queued_limit = 48; // One second of PCM at 48 kHz.
    constexpr unsigned startup_grains = 24; // Half-second reserve before AudioOut starts.
    auto* audio_buffers = static_cast<int16_t*>(memalign(64, (video ? ring_slots : 2) * block_samples * sizeof(int16_t)));
    unsigned next_buffer = 0;
    int64_t audio_start_us = 0;
    bool first_frame = true;
    bool first_packet = true, first_pcm = true, first_output = true;
    struct AudioRing {
        pthread_mutex_t mutex;
        int16_t* storage = nullptr;
        unsigned head = 0, tail = 0, queued = 0;
        int frames[ring_slots]{};
        int port = -1;
        int64_t* start_us = nullptr;
        std::atomic<bool> done{false}, stop{false}, failed{false};
        std::atomic<unsigned> grains{0}, empty_waits{0};
        std::atomic<int64_t> maximum_gap_us{0};
        unsigned peak_queued = 0;
        std::atomic<bool>* player_stop = nullptr;
        std::atomic<bool>* playing = nullptr;
        std::atomic<unsigned>* generation = nullptr;
        std::atomic<int64_t>* samples = nullptr;
        std::atomic<int64_t>* clock = nullptr;
        std::atomic<bool>* clock_ready = nullptr;
        unsigned gen = 0;
        const void* trace_player = nullptr;
        int64_t trace_origin = 0;
    } ring;
    pthread_mutex_init(&ring.mutex, nullptr);
    ring.storage = audio_buffers;
    ring.start_us = &audio_start_us;
    ring.player_stop = &stop_;
    ring.playing = &playing_;
    ring.generation = &generation_;
    ring.samples = &samples_;
    ring.clock = &clock_us_;
    ring.clock_ready = &clock_ready_;
    ring.gen = gen;
    ring.trace_player = this;
    ring.trace_origin = trace_origin;
    pthread_t output_thread{};
    bool output_started = false;
    auto current = [&] { return !stop_ && generation_ == gen; };
    auto wait = [&] { while (current() && !playing_) sceKernelDelayThread(10000); return current(); };
    auto submit = [&](const int16_t* data, int frames) {
        if (!wait()) return false;
        if (video) {
            while (current() && !ring.failed) {
                pthread_mutex_lock(&ring.mutex);
                if (ring.queued < queued_limit) {
                    unsigned slot = ring.tail;
                    int16_t* buffer = audio_buffers + slot * block_samples;
                    std::memcpy(buffer, data, frames * 2 * sizeof(int16_t));
                    std::fill(buffer + frames * 2, buffer + block_samples, 0);
                    ring.frames[slot] = frames;
                    ring.tail = (ring.tail + 1) % ring_slots;
                    ++ring.queued;
                    ring.peak_queued = std::max(ring.peak_queued, ring.queued);
                    pthread_mutex_unlock(&ring.mutex);
                    return true;
                }
                pthread_mutex_unlock(&ring.mutex);
                sceKernelDelayThread(1000);
            }
            if (ring.failed) error = i18n::text("voice.output_failed");
            return false;
        }
        int16_t* buffer = audio_buffers + next_buffer * block_samples;
        std::memcpy(buffer, data, frames * 2 * sizeof(int16_t));
        std::fill(buffer + frames * 2, buffer + block_samples, 0);
        if (first_output) trace("first_output_begin");
        const int output_result = sceAudioOutOutput(port, buffer);
        if (first_output) { trace("first_output_end", output_result); first_output = false; }
        if (output_result < 0) { error = i18n::text("voice.output_failed"); return false; }
        next_buffer ^= 1;
        pthread_mutex_lock(&mutex_);
        if (current()) {
            samples_ += frames;
            int rest = std::max(0, sceAudioOutGetRestSample(port));
            clock_us_ = audio_start_us + std::max<int64_t>(0, samples_.load() - rest) * 1000000 / 48000;
            clock_ready_ = true;
        }
        pthread_mutex_unlock(&mutex_);
        return true;
    };
    // Streaming PCM keeps memory bounded even for long recordings.
    std::vector<int16_t> pending;
    auto output = [&](const int16_t* data, int frames) {
        if (frames <= 0) return true;
        pending.insert(pending.end(), data, data + frames * 2);
        size_t offset = 0;
        while (pending.size() - offset >= block_samples) {
            if (!submit(pending.data() + offset, block_frames)) return false;
            offset += block_samples;
        }
        pending.erase(pending.begin(), pending.begin() + offset);
        return true;
    };
    do {
        trace("file_info_begin");
        const int64_t file_size = stream ? stream->snapshot().size : vault::plain_size(path); // sealed files report their plain size
        trace("file_info_end");
        if (file_size <= 0 || file_size > ((video || music) ? 128 : 20) * 1024 * 1024) { error = i18n::text("voice.unavailable"); break; }
        if (!packet || !frame || !audio_buffers) { error = i18n::text("voice.memory"); break; }
        if (music || video || stream) {
            format = avformat_alloc_context();
            if (!format) { error = i18n::text("voice.memory"); break; }
            format->probesize = 256 * 1024;
            format->max_analyze_duration = 2 * AV_TIME_BASE;
            format->interrupt_callback = {[](void* opaque) -> int {
                auto* job = static_cast<ProbeJob*>(opaque);
                return job->player->stop_ || job->player->generation_ != job->generation;
            }, &probe};
        }
        AVDictionary* options = nullptr;
        av_dict_set(&options, "protocol_whitelist", "file", 0);
        av_dict_set(&options, "format_whitelist", music ? "mp3,aac,mov,mp4,m4a,3gp,3g2,mj2,ogg,flac,wav" : video ? media_decode::video_formats : "ogg", 0);
        if (!format && vault::is_sealed(path)) format = avformat_alloc_context();
        const auto stream_state = stream ? stream->snapshot() : download_stream::State::Snapshot{};
        const std::string input_path = stream ? stream_state.path : path;
        const bool growing = stream && !(stream_state.complete && vault::is_sealed(input_path));
        trace(growing ? "attach_download_begin" : "attach_file_begin");
        const int sealed = growing ? growing_input.attach(format, stream) : format ? sealed_input.attach(format, input_path) : 0;
        trace("attach_end", sealed);
        trace("open_input_begin");
        result = sealed < 0 ? AVERROR(EIO)
                            : avformat_open_input(&format, sealed ? input_path.c_str() : ("file:" + input_path).c_str(), nullptr, &options);
        av_dict_free(&options);
        trace("open_input_end", result);
        if (result < 0) break;
        // Stream analysis takes tens of seconds on Vita; raw audio parameters come from decoded frames.
        int stream = -1;
        for (unsigned i = 0; i < format->nb_streams; ++i)
            if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { stream = i; break; }
        if (stream < 0) { if (!video) error = i18n::text("voice.no_stream"); break; }
        // Skip pictures (including music cover art) and unused tracks before reading audio packets.
        if (video || music) for (unsigned i = 0; i < format->nb_streams; ++i)
            if ((int)i != stream) format->streams[i]->discard = AVDISCARD_ALL;
        AVCodecID id = format->streams[stream]->codecpar->codec_id;
        bool supported = id == AV_CODEC_ID_OPUS || ((video || music) && (id == AV_CODEC_ID_AAC || id == AV_CODEC_ID_VORBIS));
        if (music) supported = supported || id == AV_CODEC_ID_MP3 || id == AV_CODEC_ID_FLAC || id == AV_CODEC_ID_VORBIS ||
            id == AV_CODEC_ID_PCM_S16LE || id == AV_CODEC_ID_PCM_S24LE || id == AV_CODEC_ID_PCM_S32LE || id == AV_CODEC_ID_PCM_F32LE;
        if (!supported) { error = i18n::text("voice.unsupported_codec", {avcodec_get_name(id)}); break; }
        const AVCodec* decoder = avcodec_find_decoder(id);
        if (!decoder) { error = i18n::text("voice.rebuild"); break; }
        codec = avcodec_alloc_context3(decoder);
        if (!codec) { error = i18n::text("voice.memory"); break; }
        result = avcodec_parameters_to_context(codec, format->streams[stream]->codecpar);
        if (result < 0) break;
        codec->thread_count = 1;
        codec->pkt_timebase = format->streams[stream]->time_base;
        trace("codec_open_begin", id);
        result = avcodec_open2(codec, decoder, nullptr);
        trace("codec_open_end", result);
        if (result < 0) break;
        if (const int64_t start_us = start_us_.load(); start_us > 0) {
            // Fresh decoder: seek the demuxer to the last packet before the target.
            int64_t target = start_us + (format->start_time != AV_NOPTS_VALUE ? format->start_time : 0);
            trace("seek_begin");
            const int seek_result = av_seek_frame(format, stream, av_rescale_q(target, AV_TIME_BASE_Q, format->streams[stream]->time_base), AVSEEK_FLAG_BACKWARD);
            trace("seek_end", seek_result);
            if (seek_result < 0) { if (!video) samples_ = 0; }   // not seekable: play from the start
        }
        trace("output_open_begin");
        port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, block_frames, 48000, SCE_AUDIO_OUT_MODE_STEREO);
        trace("output_open_end", port);
        if (port < 0) { error = i18n::text("voice.open_output"); break; }
        if (video) {
            ring.port = port;
            auto output_entry = +[](void* opaque) -> void* {
                auto* queue = static_cast<AudioRing*>(opaque);
                cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Audio);
                int priority = sceKernelGetThreadCurrentPriority();
                if (priority > 34)
                    sceKernelChangeThreadPriority(sceKernelGetThreadId(), priority - 2);
                int64_t last_output_us = 0;
                while (!queue->stop && !*queue->player_stop && *queue->generation == queue->gen) {
                    if (!queue->grains && !queue->done) {
                        pthread_mutex_lock(&queue->mutex);
                        bool primed = queue->queued >= startup_grains;
                        pthread_mutex_unlock(&queue->mutex);
                        if (!primed) { sceKernelDelayThread(1000); continue; }
                    }
                    if (queue->done) {
                        pthread_mutex_lock(&queue->mutex);
                        bool empty = queue->queued == 0;
                        pthread_mutex_unlock(&queue->mutex);
                        if (empty) break;
                    }
                    if (!*queue->playing) { sceKernelDelayThread(2000); continue; }
                    pthread_mutex_lock(&queue->mutex);
                    bool available = queue->queued != 0;
                    unsigned slot = queue->head;
                    int frames = available ? queue->frames[slot] : 0;
                    if (available) {
                        queue->head = (queue->head + 1) % ring_slots;
                        --queue->queued;
                    }
                    pthread_mutex_unlock(&queue->mutex);
                    if (!available) {
                        if (queue->done) break;
                        if (queue->grains) ++queue->empty_waits;
                        sceKernelDelayThread(1000);
                        continue;
                    }
                    int16_t* buffer = queue->storage + slot * block_samples;
                    if (!queue->grains) audio_startup_trace::record(queue->trace_player, queue->gen, "first_output_begin", 0, queue->trace_origin);
                    const int output_result = sceAudioOutOutput(queue->port, buffer);
                    if (!queue->grains) audio_startup_trace::record(queue->trace_player, queue->gen, "first_output_end", output_result, queue->trace_origin);
                    if (output_result < 0) {
                        queue->failed = true;
                        break;
                    }
                    int64_t output_us = sceKernelGetSystemTimeWide();
                    if (last_output_us) {
                        int64_t gap = output_us - last_output_us;
                        if (gap > queue->maximum_gap_us) queue->maximum_gap_us = gap;
                    }
                    last_output_us = output_us;
                    ++queue->grains;
                    if (!*queue->player_stop && *queue->generation == queue->gen) {
                        int64_t submitted = queue->samples->fetch_add(frames) + frames;
                        int rest = std::max(0, sceAudioOutGetRestSample(queue->port));
                        queue->clock->store(*queue->start_us + std::max<int64_t>(0, submitted - rest) * 1000000 / 48000);
                        queue->clock_ready->store(true);
                    }
                }
                return nullptr;
            };
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 128 * 1024);
            int thread_rc = pthread_create(&output_thread, &attr, output_entry, &ring);
            pthread_attr_destroy(&attr);
            if (thread_rc != 0) { error = i18n::text("voice.worker"); break; }
            output_started = true;
        }
        bool eof = false, done = false;
        trace("first_packet_begin");
        while (current() && !done && error.empty()) {
            if (!wait()) break;
            result = av_read_frame(format, packet);
            if (result == AVERROR_EOF) { eof = true; result = avcodec_send_packet(codec, nullptr); }
            else if (result < 0) break;
            else {
                if (packet->stream_index != stream) { av_packet_unref(packet); continue; }
                if (first_packet) { trace("first_audio_packet"); first_packet = false; }
                result = avcodec_send_packet(codec, packet); av_packet_unref(packet);
            }
            if (result < 0) break;
            while (current()) {
                result = avcodec_receive_frame(codec, frame);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) { result = 0; break; }
                if (result < 0) break;
                if (first_frame) {
                    trace("first_decoded_frame", frame->sample_rate);
                    trace("decoded_channels", frame->ch_layout.nb_channels);
                    trace("decoded_sample_format", frame->format);
                    first_frame = false;
                    if (video && frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                        audio_start_us = av_rescale_q(frame->best_effort_timestamp, codec->pkt_timebase, AVRational{1,1000000});
                        if (format->start_time != AV_NOPTS_VALUE) audio_start_us -= format->start_time;
                        audio_start_us = std::max<int64_t>(0, audio_start_us);
                    }
                }
                if (resampler && (frame->format != resampler_format || frame->sample_rate != resampler_rate ||
                                  av_channel_layout_compare(&frame->ch_layout, &resampler_layout) != 0)) {
                    // swr_convert trusts the frame to match its configuration: a stereo-built resampler
                    // fed a mono planar frame reads a missing plane. Flush the old one and rebuild.
                    int16_t tail[8192]; uint8_t* dest = reinterpret_cast<uint8_t*>(tail);
                    int n;
                    while ((n = swr_convert(resampler, &dest, 4096, nullptr, 0)) > 0 && current())
                        if (!output(tail, n)) break;
                    swr_free(&resampler);
                    av_channel_layout_uninit(&resampler_layout);
                }
                if (!resampler) {
                    if (first_pcm) trace("resampler_init_begin");
                    if (frame->sample_rate <= 0 || frame->ch_layout.nb_channels <= 0) { result = AVERROR_INVALIDDATA; break; }
                    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                    result = swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, 48000,
                        &frame->ch_layout, (AVSampleFormat)frame->format, frame->sample_rate, 0, nullptr);
                    if (result >= 0) result = swr_init(resampler);
                    if (result >= 0) result = av_channel_layout_copy(&resampler_layout, &frame->ch_layout);
                    if (first_pcm) trace("resampler_init_end", result);
                    if (result < 0) break;
                    resampler_format = frame->format; resampler_rate = frame->sample_rate;
                }
                int capacity = swr_get_out_samples(resampler, frame->nb_samples);
                if (capacity < 0 || capacity > 48000) { error = i18n::text("voice.frame_size"); break; }
                std::vector<int16_t> pcm(capacity * 2);
                uint8_t* dest = reinterpret_cast<uint8_t*>(pcm.data());
                if (first_pcm) trace("first_pcm_begin");
                result = swr_convert(resampler, &dest, capacity, (const uint8_t**)frame->extended_data, frame->nb_samples);
                if (first_pcm) { trace("first_pcm_end", result); first_pcm = false; }
                av_frame_unref(frame);
                if (result < 0 || !output(pcm.data(), result)) { done = true; break; }
            }
            if (result < 0 || eof) break;
        }
        if (current() && eof && result >= 0 && error.empty()) {
            if (resampler) {
                int16_t tail[8192]; uint8_t* dest = reinterpret_cast<uint8_t*>(tail);
                int n;
                while ((n = swr_convert(resampler, &dest, 4096, nullptr, 0)) > 0 && current())
                    if (!output(tail, n)) break;
                if (n < 0) result = n;
            }
            if (!pending.empty() && wait() && error.empty()) {
                submit(pending.data(), static_cast<int>(pending.size() / 2));
            }
        }
    } while (false);
    trace(!current() ? "decode_cancelled" : result < 0 || !error.empty() ? "decode_error" : "decode_loop_end", result);
    ring.done = true;
    if (!current() || !error.empty() || result < 0) ring.stop = true;
    if (output_started) pthread_join(output_thread, nullptr);
    if (ring.failed && error.empty()) error = i18n::text("voice.output_failed");
    if (video && generation_ == gen) {
        FILE* log = diag_log::open(diag_log::Kind::Audio, "ux0:data/vitagram/audio-video.log", "w");
        if (log) {
            std::fprintf(log, "VitaGram %s\nqueue_limit=%u startup_grains=%u grains=%u empty_waits=%u max_gap_us=%lld peak_queued=%u samples=%lld failed=%d\n",
                VITAGRAM_VERSION, queued_limit, startup_grains, ring.grains.load(), ring.empty_waits.load(),
                (long long)ring.maximum_gap_us.load(), ring.peak_queued,
                (long long)samples_.load(), ring.failed ? 1 : 0);
            std::fclose(log);
        }
    }
    trace("cleanup_begin");
    if (port >= 0) { sceAudioOutOutput(port, nullptr); sceAudioOutReleasePort(port); }
    pthread_mutex_destroy(&ring.mutex);
    std::free(audio_buffers);
    swr_free(&resampler); av_channel_layout_uninit(&resampler_layout); av_frame_free(&frame); av_packet_free(&packet);
    avcodec_free_context(&codec); avformat_close_input(&format);
    sealed_input.release();
    growing_input.release();
    trace("cleanup_end");
    pthread_mutex_lock(&mutex_);
    if (generation_ == gen && !stop_) {
        if (clock_ready_) clock_us_ = audio_start_us + samples_.load() * 1000000 / 48000;
        if (error.empty() && result < 0) { char buffer[128]; av_strerror(result, buffer, sizeof(buffer)); error = i18n::text("voice.error", {buffer}); }
        status_ = error; playing_ = false; finished_ = true;
    }
    pthread_mutex_unlock(&mutex_);
}
