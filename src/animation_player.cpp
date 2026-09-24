#include "animation_player.h"
#include "cpu_scheduler.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <zlib.h>
#ifdef VITAGRAM_RLOTTIE
#include <rlottie_capi.h>
#endif
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {
std::string decoder_error(const char* stage, int code) {
    char description[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, description, sizeof(description));
    return std::string(stage) + ": " + std::to_string(code) + " " + description;
}

// Only the latest failure is retained; no chat text, paths or credentials are logged.
void log_decoder_error(const std::string& message, AVCodecContext* codec, AVPacket* packet,
                       AVFrame* frame, unsigned packets, unsigned frames) {
    std::fprintf(stderr, "Animation: %s\n", message.c_str());
    FILE* log = std::fopen("ux0:data/vitagram/animation-error.log", "w");
    if (!log) return;
    std::fprintf(log, "VitaGram %s\nError: %s\nPackets: %u Frames: %u\nFFmpeg: %s\nConfiguration: %s\n",
        VITAGRAM_VERSION, message.c_str(), packets, frames, av_version_info(), avcodec_configuration());
    if (codec) std::fprintf(log, "Codec: %s profile=%d level=%d size=%dx%d pix_fmt=%d time_base=%d/%d\n",
        avcodec_get_name(codec->codec_id), codec->profile, codec->level, codec->width, codec->height,
        (int)codec->pix_fmt, codec->pkt_timebase.num, codec->pkt_timebase.den);
    if (packet) std::fprintf(log, "Packet: size=%d pts=%lld dts=%lld duration=%lld flags=%d\n",
        packet->size, (long long)packet->pts, (long long)packet->dts, (long long)packet->duration, packet->flags);
    if (frame) std::fprintf(log, "Frame: size=%dx%d pix_fmt=%d pts=%lld best_pts=%lld duration=%lld strides=%d,%d,%d\n",
        frame->width, frame->height, frame->format, (long long)frame->pts, (long long)frame->best_effort_timestamp,
        (long long)frame->duration, frame->linesize[0], frame->linesize[1], frame->linesize[2]);
    std::fclose(log);
}
}

AnimationPlayer::~AnimationPlayer() { shutdown(); pthread_mutex_destroy(&mutex_); }

void AnimationPlayer::setSource(const std::string& path, bool video, bool round) {
    pthread_mutex_lock(&mutex_);
    if (path == source_ && video_ == video && round_ == round) { pthread_mutex_unlock(&mutex_); return; }
    source_ = path;
    video_ = video; round_ = round;
    paused_ = video; finished_ = false; position_us_ = 0;
    ++generation_;
    ready_ = false;
    status_ = path.empty() ? "" : video ? "X Play" : "Preparing GIF...";
    pthread_mutex_unlock(&mutex_);
    audio_.setSource(video ? path : "", video);
    if (texture_) { vita2d_wait_rendering_done(); vita2d_free_texture(texture_); texture_ = nullptr; }
    if (!path.empty() && !started_) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
        int rc = pthread_create(&thread_, &attr, entry, this);
        pthread_attr_destroy(&attr);
        if (!rc) started_ = true;
        else setStatus(generation_, "Unable to start animation decoder");
    }
}

void AnimationPlayer::toggle() {
    if (!video_ || source_.empty() || !started_) return;
    pthread_mutex_lock(&mutex_);
    bool replay = finished_;
    if (replay) { ++generation_; finished_ = false; ready_ = false; position_us_ = 0; status_.clear(); }
    if (replay) { audio_.setSource(""); audio_.setSource(source_, true); }
    paused_ = replay ? false : !paused_.load();
    bool play = !paused_;
    pthread_mutex_unlock(&mutex_);
    audio_.setPlaying(play);
}

void AnimationPlayer::upload() {
    if (video_ && finished_) audio_.setPlaying(false);
    pthread_mutex_lock(&mutex_);
    bool pending = ready_;
    pthread_mutex_unlock(&mutex_);
    if (!pending) return;
    vita2d_wait_rendering_done();
    pthread_mutex_lock(&mutex_);
    if (ready_) {
        if (texture_ && (vita2d_texture_get_width(texture_) != (unsigned)width_ ||
                         vita2d_texture_get_height(texture_) != (unsigned)height_)) {
            vita2d_free_texture(texture_); texture_ = nullptr;
        }
        if (!texture_) texture_ = vita2d_create_empty_texture(width_, height_);
        if (texture_) {
            auto* dest = static_cast<unsigned char*>(vita2d_texture_get_datap(texture_));
            unsigned stride = vita2d_texture_get_stride(texture_);
            for (int y = 0; y < height_; ++y)
                std::memcpy(dest + y * stride, pixels_.data() + y * width_ * 4, width_ * 4);
            vita2d_texture_set_filters(texture_, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        } else status_ = "Not enough GPU memory for GIF";
        ready_ = false;
    }
    pthread_mutex_unlock(&mutex_);
}

std::string AnimationPlayer::status() {
    pthread_mutex_lock(&mutex_);
    std::string result = status_;
    if (video_ && (result.empty() || result == "X Play" || result == "Video - playing"))
        result = finished_ ? "Finished - X replay" : paused_ ? "X Play / Pause" : "Video - playing / X pause";
    pthread_mutex_unlock(&mutex_);
    if (video_ && !audio_.error().empty()) result += " | " + audio_.error();
    return result;
}
void AnimationPlayer::setStatus(unsigned generation, const std::string& text) {
    pthread_mutex_lock(&mutex_);
    if (generation == generation_) status_ = text;
    pthread_mutex_unlock(&mutex_);
}
void AnimationPlayer::shutdown() {
    stopping_ = true;
    if (started_) { pthread_join(thread_, nullptr); started_ = false; }
    audio_.shutdown();
    if (texture_) { vita2d_wait_rendering_done(); vita2d_free_texture(texture_); texture_ = nullptr; }
    pixels_.clear();
}
int AnimationPlayer::interrupted(void* opaque) {
    auto* job = static_cast<Job*>(opaque);
    return job->player->stopping_ || job->generation != job->player->generation_;
}
void* AnimationPlayer::entry(void* opaque) { static_cast<AnimationPlayer*>(opaque)->run(); return nullptr; }
void AnimationPlayer::run() {
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Video);
    unsigned handled = 0;
    while (!stopping_) {
        pthread_mutex_lock(&mutex_);
        unsigned generation = generation_;
        std::string path = source_;
        bool video = video_, round = round_;
        pthread_mutex_unlock(&mutex_);
        if (generation != handled) {
            handled = generation;
            if (!path.empty()) decode(path, generation, video, round);
        } else sceKernelDelayThread(10000);
    }
}

void AnimationPlayer::decodeTgs(const std::string& path, unsigned generation) {
#ifdef VITAGRAM_RLOTTIE
    gzFile input = gzopen(path.c_str(), "rb");
    if (!input) { setStatus(generation, "Cannot open TGS"); return; }
    std::string data;
    char block[8192]; int bytes = 0;
    while (!stopping_ && generation == generation_ && (bytes = gzread(input, block, sizeof(block))) > 0) {
        data.append(block, bytes);
        if (data.size() > 2*1024*1024) break;
    }
    int closed = gzclose(input);
    if (stopping_ || generation != generation_) return;
    if (bytes < 0 || closed != Z_OK || data.empty() || data.size() > 2*1024*1024) {
        setStatus(generation, "Invalid or oversized TGS"); return;
    }
    Lottie_Animation* animation = lottie_animation_from_data(data.c_str(), path.c_str(), "");
    if (!animation) { setStatus(generation, "TGS parsing failed"); return; }
    size_t frames = lottie_animation_get_totalframe(animation);
    double fps = lottie_animation_get_framerate(animation);
    size_t source_w = 0, source_h = 0;
    lottie_animation_get_size(animation, &source_w, &source_h);
    if (!frames || frames > 1800 || !std::isfinite(fps) || fps <= 0 || fps > 120 ||
        !source_w || !source_h || source_w > 4096 || source_h > 4096) {
        lottie_animation_destroy(animation); setStatus(generation, "Unsupported TGS dimensions/timing"); return;
    }
    double scale = 256.0/std::max(source_w, source_h);
    int w = std::max(1, (int)(source_w*scale)), h = std::max(1, (int)(source_h*scale));
    std::vector<uint32_t> argb(w*h);
    std::vector<unsigned char> rgba(w*h*4);
    int64_t start = sceKernelGetSystemTimeWide();
    while (!stopping_ && generation == generation_) {
        int64_t tick = sceKernelGetSystemTimeWide();
        size_t frame = (size_t)((tick-start)*fps/1000000.0) % frames;
        std::fill(argb.begin(), argb.end(), 0);
        lottie_animation_render(animation, frame, argb.data(), w, h, w*4);
        for (size_t i=0; i<argb.size(); ++i) {
            unsigned pixel = argb[i], alpha = pixel>>24;
            // rlottie outputs premultiplied ARGB; vita2d expects straight RGBA.
            rgba[i*4] = alpha ? std::min(255u, ((pixel>>16)&255)*255/alpha) : 0;
            rgba[i*4+1] = alpha ? std::min(255u, ((pixel>>8)&255)*255/alpha) : 0;
            rgba[i*4+2] = alpha ? std::min(255u, (pixel&255)*255/alpha) : 0;
            rgba[i*4+3] = alpha;
        }
        pthread_mutex_lock(&mutex_);
        if (generation == generation_) { pixels_.swap(rgba); width_ = w; height_ = h; ready_ = true; status_ = "TGS - playing"; }
        pthread_mutex_unlock(&mutex_);
        rgba.resize(w*h*4);
        int64_t wait = 33333-(sceKernelGetSystemTimeWide()-tick);
        sceKernelDelayThread(wait > 0 ? (unsigned)wait : 1000);
    }
    lottie_animation_destroy(animation);
#else
    setStatus(generation, "TGS needs rlottie: run build-sticker-renderer.sh");
#endif
}

void AnimationPlayer::decode(const std::string& path, unsigned generation, bool video, bool round) {
    cpu_scheduler::Media reservation(cpu_scheduler::Role::Video);
    Job job{this, generation};
    auto early_error = [&](const std::string& text) {
        pthread_mutex_lock(&mutex_);
        if (generation == generation_) { status_ = text; finished_ = true; }
        pthread_mutex_unlock(&mutex_);
    };
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) { early_error("Cannot open media file"); return; }
    unsigned char signature[2]{};
    size_t signature_size = std::fread(signature, 1, 2, file);
    if (!video && signature_size == 2 && signature[0] == 0x1f && signature[1] == 0x8b) {
        std::fclose(file); decodeTgs(path, generation); return;
    }
    int seek_rc = std::fseek(file, 0, SEEK_END);
    long file_size = std::ftell(file); std::fclose(file);
    if (seek_rc || file_size <= 0 || file_size > (video ? 128 : 20) * 1024 * 1024) {
        early_error(video ? "Video limit: 128 MiB" : "GIF limit: 20 MiB"); return;
    }
    AVFormatContext* format = avformat_alloc_context();
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* scaler = nullptr;
    unsigned packets_read = 0, frames_decoded = 0;
    auto cleanup = [&]() {
        sws_freeContext(scaler); av_frame_free(&frame); av_packet_free(&packet);
        avcodec_free_context(&codec); avformat_close_input(&format);
    };
    auto fail = [&](const std::string& message) {
        // Closing/switching a post is not a decoder error.
        if (!interrupted(&job)) {
            log_decoder_error(message, codec, packet, frame, packets_read, frames_decoded);
            setStatus(generation, message);
            pthread_mutex_lock(&mutex_);
            if (generation == generation_) finished_ = true;
            pthread_mutex_unlock(&mutex_);
        }
        cleanup();
    };
    if (!format) { fail("Not enough memory for animation"); return; }
    format->interrupt_callback = {interrupted, &job};
    format->probesize = 256 * 1024;
    format->max_analyze_duration = 2 * AV_TIME_BASE;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "format_whitelist", "gif,mov,mp4,m4a,3gp,3g2,mj2", 0);
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "ignore_loop", "1", 0); // Loop explicitly after draining the decoder.
    // ux0: is a Vita mount, not an FFmpeg protocol.
    int rc = avformat_open_input(&format, ("file:" + path).c_str(), nullptr, &options);
    av_dict_free(&options);
    // GIF/MOV headers provide stream parameters; do not decode probe frames before validating dimensions.
    if (rc < 0) { fail(decoder_error("Open", rc)); return; }
    int stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0) { fail(decoder_error("Video stream", stream_index)); return; }
    AVStream* stream = format->streams[stream_index];
    auto* parameters = stream->codecpar;
    if (parameters->codec_id != AV_CODEC_ID_H264 && (video || parameters->codec_id != AV_CODEC_ID_GIF)) {
        fail(video ? "Supported video: H.264 MP4" : "Supported animations: GIF and H.264 MP4"); return;
    }
    auto dimensions_ok = [](int w, int h) { return w > 0 && h > 0 && w <= 1280 && h <= 1280 && (int64_t)w * h <= 1280 * 720; };
    if (!dimensions_ok(parameters->width, parameters->height)) { fail("Animation exceeds 720p pixel limit"); return; }
    const AVCodec* decoder = avcodec_find_decoder(parameters->codec_id);
    codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!codec) { fail("Animation decoder unavailable"); return; }
    rc = avcodec_parameters_to_context(codec, parameters);
    if (rc < 0) { fail(decoder_error("Parameters", rc)); return; }
    codec->thread_count = 1;
    codec->pkt_timebase = stream->time_base;
    codec->max_pixels = 1280 * 720;
    rc = avcodec_open2(codec, decoder, nullptr);
    if (rc < 0) { fail(decoder_error("Decoder init", rc)); return; }
    packet = av_packet_alloc(); frame = av_frame_alloc();
    if (!packet || !frame) { fail("Not enough memory for GIF frames"); return; }
    AVRational rate = av_guess_frame_rate(format, stream, nullptr);
    int64_t fallback_delay = rate.num > 0 && rate.den > 0 ? av_rescale_q(1, av_inv_q(rate), AVRational{1,1000000}) : 100000;
    fallback_delay = std::max<int64_t>(10000, std::min<int64_t>(1000000, fallback_delay));
    std::vector<unsigned char> rgba;
    int64_t origin_pts = AV_NOPTS_VALUE, origin_wall = 0, next_wall = 0, last_output = 0;
    bool had_frame = false;
    std::string frame_error;
    int64_t video_wall = 0, tail_wall = 0, tail_position = 0, video_end = 0;
    // Audio is the master clock. Silent clips use the monotonic wall clock;
    // after a shorter audio track ends, continue its timeline without freezing.
    auto video_time = [&]() -> int64_t {
        int64_t now = sceKernelGetSystemTimeWide();
        if (!video_wall) video_wall = now;
        if (!audio_.finished()) {
            if (!audio_.clockReady()) { video_wall = now; return -1; }
            return audio_.positionUs();
        }
        if (audio_.clockReady()) {
            if (!tail_wall) { tail_wall = now; tail_position = audio_.positionUs(); }
            return tail_position + now - tail_wall;
        }
        return now - video_wall;
    };
    auto wait_video = [&](int64_t due) -> int64_t {
        while (!interrupted(&job)) {
            if (paused_) {
                int64_t start = sceKernelGetSystemTimeWide();
                while (paused_ && !interrupted(&job)) sceKernelDelayThread(10000);
                int64_t delay = sceKernelGetSystemTimeWide() - start;
                if (video_wall) video_wall += delay;
                if (tail_wall) tail_wall += delay;
                continue;
            }
            int64_t time = video_time();
            if (time >= due) return time;
            sceKernelDelayThread(5000);
        }
        return due;
    };
    // Called after each accepted packet, including the null drain packet.
    auto receive = [&]() -> bool {
        frame_error.clear();
        while (!interrupted(&job)) {
            int result = avcodec_receive_frame(codec, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
            if (result < 0) { frame_error = decoder_error("Receive frame", result); return false; }
            ++frames_decoded;
            if (!dimensions_ok(frame->width, frame->height)) {
                frame_error = "Frame size: " + std::to_string(frame->width) + "x" + std::to_string(frame->height);
                return false;
            }
            int64_t now = sceKernelGetSystemTimeWide();
            if (!origin_wall) { origin_wall = now; origin_pts = frame->best_effort_timestamp; next_wall = now; }
            int64_t due = next_wall;
            if (origin_pts != AV_NOPTS_VALUE && frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                int64_t offset = av_rescale_q(frame->best_effort_timestamp - origin_pts, stream->time_base, AVRational{1,1000000});
                if (offset < 0 || offset > (video ? 86400LL : 300LL) * 1000000) {
                    frame_error = "Frame timestamp: " + std::to_string(offset) + " us";
                    return false;
                }
                due = origin_wall + offset;
            }
            int64_t video_due = due - origin_wall;
            int64_t duration = frame->duration > 0 ? av_rescale_q(frame->duration, stream->time_base, AVRational{1,1000000}) : fallback_delay;
            duration = std::max<int64_t>(10000, std::min<int64_t>(60000000, duration));
            next_wall = due + duration;
            if (video) {
                if (frame->best_effort_timestamp != AV_NOPTS_VALUE && format->start_time != AV_NOPTS_VALUE)
                    video_due = std::max<int64_t>(0, av_rescale_q(frame->best_effort_timestamp, stream->time_base, AVRational{1,1000000}) - format->start_time);
                video_end = video_due + duration;
                int64_t clock = wait_video(video_due);
                now = sceKernelGetSystemTimeWide();
                if (interrupted(&job)) return true;
                pthread_mutex_lock(&mutex_);
                if (generation == generation_) position_us_ = video_due;
                pthread_mutex_unlock(&mutex_);
                // Drop late display frames, never compressed packets or decoder references.
                if (had_frame && clock - video_due > 100000) { av_frame_unref(frame); continue; }
            } else {
                while (!interrupted(&job) && (now = sceKernelGetSystemTimeWide()) < due)
                    sceKernelDelayThread((unsigned)std::min<int64_t>(10000, due - now));
            }
            if (interrupted(&job)) return true;
            had_frame = true;
            // Decode/composite every frame, but upload at most 30 fps.
            if (!last_output || now - last_output >= 33000) {
                float scale = std::min(1.0f, std::min(640.0f / frame->width, 360.0f / frame->height));
                int w = std::max(1, (int)(frame->width * scale)), h = std::max(1, (int)(frame->height * scale));
                // FFmpeg 7 ARM unscaled YUV->RGBA NEON wrappers return 0 after
                // conversion, not the row count required by sws_scale's API.
                // ACCURATE_RND bypasses those wrappers; keep strict row checking.
                scaler = sws_getCachedContext(scaler, frame->width, frame->height, (AVPixelFormat)frame->format,
                    w, h, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
                if (!scaler) {
                    const char* pixel_format = av_get_pix_fmt_name((AVPixelFormat)frame->format);
                    frame_error = std::string("Scaler init: ") + (pixel_format ? pixel_format : "unknown pixel format");
                    return false;
                }
                rgba.resize(w * h * 4);
                uint8_t* data[] = {rgba.data(), nullptr, nullptr, nullptr};
                int strides[] = {w * 4, 0, 0, 0};
                int rows = sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, data, strides);
                if (rows != h) {
                    frame_error = rows < 0 ? decoder_error("Scale frame", rows) :
                        "Scale rows: " + std::to_string(rows) + "/" + std::to_string(h);
                    return false;
                }
                if (round) {
                    // Video notes are square; crop their displayed pixels to a circle.
                    float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
                    float radius = std::min(w, h) * 0.5f;
                    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                        if ((x-cx)*(x-cx) + (y-cy)*(y-cy) > radius*radius)
                            rgba[(y*w+x)*4+3] = 0;
                }
                pthread_mutex_lock(&mutex_);
                if (generation == generation_) {
                    pixels_.swap(rgba); width_ = w; height_ = h; ready_ = true; status_ = video ? "Video - playing" : "GIF - playing";
                }
                pthread_mutex_unlock(&mutex_);
                last_output = now;
            }
            av_frame_unref(frame);
        }
        return true;
    };
    while (!interrupted(&job)) {
        rc = av_read_frame(format, packet);
        if (rc == AVERROR_EOF) {
            rc = avcodec_send_packet(codec, nullptr);
            if (rc < 0) { fail(decoder_error("Send EOF", rc)); return; }
            if (!receive()) { fail(frame_error); return; }
            if (!had_frame) { fail("Animation has no decoded frames"); return; }
            if (video) {
                wait_video(video_end);
                pthread_mutex_lock(&mutex_);
                if (generation == generation_) { position_us_ = video_end; finished_ = true; paused_ = true; }
                pthread_mutex_unlock(&mutex_);
                break;
            }
            while (!interrupted(&job) && sceKernelGetSystemTimeWide() < next_wall) sceKernelDelayThread(10000);
            if (interrupted(&job)) break;
            rc = av_seek_frame(format, stream_index, stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time, AVSEEK_FLAG_BACKWARD);
            if (rc < 0) {
                fail(decoder_error("Loop seek", rc)); return;
            }
            avcodec_flush_buffers(codec);
            origin_pts = AV_NOPTS_VALUE; origin_wall = next_wall = last_output = 0; had_frame = false;
            continue;
        }
        if (rc < 0) { fail(decoder_error("Read packet", rc)); return; }
        if (packet->stream_index == stream_index) {
            ++packets_read;
            rc = avcodec_send_packet(codec, packet);
            if (rc < 0) { fail(decoder_error("Send packet", rc)); return; }
            if (!receive()) { fail(frame_error); return; }
            av_packet_unref(packet);
        } else av_packet_unref(packet);
    }
    cleanup();
}
