#include "diag_log.h"
#include "performance.h"
#include "i18n.h"
#include "animation_player.h"
#include "media_decode_policy.h"
#include "portrait_video.h"
#include "vault.h"
#include "vault_avio.h"
#include "download_avio.h"
#include "media_seek.h"
#include "circle_mask.h"
#include "cpu_scheduler.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <mutex>
#include <optional>
#include <zlib.h>
#ifdef VITAGRAM_RLOTTIE
#include <rlottie_capi.h>
#include "tgs_policy.h"
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
thread_local const void* logged_codec = nullptr;
thread_local char logged_decoder_error[1024]{};
thread_local size_t logged_decoder_error_length = 0;
void capture_ffmpeg_error(void* context, int level, const char* format, va_list args) {
    if (logged_codec && context == logged_codec && level <= AV_LOG_ERROR) {
        va_list copy;
        va_copy(copy, args);
        char line[256]{};
        std::vsnprintf(line, sizeof(line), format, copy);
        va_end(copy);
        size_t length = std::strlen(line);
        size_t available = sizeof(logged_decoder_error) - logged_decoder_error_length - 1;
        length = std::min(length, available);
        std::memcpy(logged_decoder_error + logged_decoder_error_length, line, length);
        logged_decoder_error_length += length;
        logged_decoder_error[logged_decoder_error_length] = '\0';
    }
    av_log_default_callback(context, level, format, args);
}
std::string decoder_error(const char* stage, int code) {
    char description[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, description, sizeof(description));
    return std::string(stage) + ": " + std::to_string(code) + " " + description;
}

// Only the latest failure is retained; no chat text, paths or credentials are logged.
void log_decoder_error(const std::string& message, AVCodecContext* codec, AVPacket* packet,
                       AVFrame* frame, unsigned packets, unsigned frames) {
    std::fprintf(stderr, "Animation: %s\n", message.c_str());
    FILE* log = diag_log::open(diag_log::Kind::Video, "ux0:data/vitagram/animation-error.log", "w");
    if (!log) return;
    std::fprintf(log, "VitaGram %s\nError: %s\nPackets: %u Frames: %u\nFFmpeg: %s\nConfiguration: %s\n",
        VITAGRAM_VERSION, message.c_str(), packets, frames, av_version_info(), avcodec_configuration());
    if (logged_decoder_error[0]) std::fprintf(log, "Decoder detail: %s\n", logged_decoder_error);
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
#if defined(VITAGRAM_RLOTTIE) && VITAGRAM_TGS_DIAGNOSTIC
bool save_tgs_frame(const std::vector<uint32_t>& pixels, int width, int height) {
    FILE* file = std::fopen("ux0:data/vitagram/tgs-render-frame.ppm", "wb");
    if (!file) return false;
    bool ok = std::fprintf(file, "P6\n%d %d\n255\n", width, height) > 0;
    for (uint32_t pixel : pixels) {
        unsigned alpha = pixel >> 24;
        // rlottie gives premultiplied ARGB. Composite against the white UI background.
        unsigned char rgb[3] = {
            static_cast<unsigned char>(std::min(255u, ((pixel >> 16) & 255u) + 255u - alpha)),
            static_cast<unsigned char>(std::min(255u, ((pixel >> 8) & 255u) + 255u - alpha)),
            static_cast<unsigned char>(std::min(255u, (pixel & 255u) + 255u - alpha))
        };
        if (ok && std::fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb)) ok = false;
    }
    if (std::fclose(file)) ok = false;
    return ok;
}
#endif
}

AnimationPlayer::~AnimationPlayer() { shutdown(); pthread_mutex_destroy(&mutex_); }

void AnimationPlayer::releaseDirectBuffer(void* opaque, uint8_t*) {
    static_cast<YuvSurface*>(opaque)->busy.store(false, std::memory_order_release);
}

int AnimationPlayer::directBuffer(AVCodecContext* codec, AVFrame* frame, int) {
    auto* player = static_cast<AnimationPlayer*>(codec->opaque);
    if (!player || frame->format != AV_PIX_FMT_NV12 || frame->width <= 0 || frame->height <= 0 ||
        frame->width > 1280 || frame->height > 1280 ||
        static_cast<int64_t>(frame->width) * frame->height > 1280 * 720) return AVERROR(EINVAL);
    const int pitch = (frame->width + 15) & ~15;
    const int height = (frame->height + 15) & ~15;
    const int bytes = av_image_get_buffer_size(AV_PIX_FMT_NV12, pitch, height, 1);
    if (bytes <= 0) return AVERROR(EINVAL);
    const unsigned allocation = (static_cast<unsigned>(bytes) + 0x3ffffu) & ~0x3ffffu;
    for (int retry = 0; retry < 100 && !player->stopping_; ++retry) {
        for (auto& surface : player->yuv_surfaces_) {
            bool free = false;
            if (!surface.busy.compare_exchange_strong(free, true, std::memory_order_acq_rel)) continue;
            if (surface.allocated < allocation) {
                if (surface.pixels) sceGxmUnmapMemory(surface.pixels);
                if (surface.uid >= 0) sceKernelFreeMemBlock(surface.uid);
                performance::cdram_video -= surface.allocated;
                surface.pixels = nullptr; surface.uid = -1; surface.allocated = 0;
                SceKernelAllocMemBlockOpt opt{};
                opt.size = sizeof(opt);
                opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
                opt.alignment = 256 * 1024;
                surface.uid = sceKernelAllocMemBlock("vitagram_h264_frame",
                    SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, allocation, &opt);
                int base_result = surface.uid >= 0 ? sceKernelGetMemBlockBase(surface.uid, &surface.pixels) : surface.uid;
                int map_result = base_result >= 0 && surface.pixels ?
                    sceGxmMapMemory(surface.pixels, allocation,
                        static_cast<SceGxmMemoryAttribFlags>(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)) : base_result;
                if (map_result >= 0) {
                    surface.allocated = allocation;
                    performance::cdram_video += allocation;
                } else {
                    FILE* log = diag_log::open(diag_log::Kind::Video, "ux0:data/vitagram/video-hardware.log", "a");
                    if (log) {
                        std::fprintf(log, "direct_alloc_failed uid=0x%08x base=0x%08x map=0x%08x bytes=%u\n",
                            (unsigned)surface.uid, (unsigned)base_result, (unsigned)map_result, allocation);
                        std::fclose(log);
                    }
                    if (surface.uid >= 0) sceKernelFreeMemBlock(surface.uid);
                    surface.uid = -1; surface.pixels = nullptr; surface.busy = false;
                    return AVERROR(ENOMEM);
                }
            }
            frame->buf[0] = av_buffer_create(static_cast<uint8_t*>(surface.pixels), surface.allocated,
                releaseDirectBuffer, &surface, 0);
            if (!frame->buf[0] || av_image_fill_arrays(frame->data, frame->linesize,
                static_cast<uint8_t*>(surface.pixels), AV_PIX_FMT_NV12, pitch, height, 1) <= 0) {
                av_buffer_unref(&frame->buf[0]);
                surface.busy = false;
                return AVERROR(ENOMEM);
            }
            frame->extended_data = frame->data;
            return 0;
        }
        sceKernelDelayThread(1000);
    }
    unsigned busy = 0;
    for (const auto& surface : player->yuv_surfaces_) if (surface.busy.load()) ++busy;
    FILE* log = diag_log::open(diag_log::Kind::Video, "ux0:data/vitagram/video-hardware.log", "a");
    if (log) {
        std::fprintf(log, "direct_pool_exhausted busy=%u capacity=%u\n", busy,
            static_cast<unsigned>(player->yuv_surfaces_.size()));
        std::fclose(log);
    }
    return AVERROR(ENOMEM);
}

void AnimationPlayer::releaseYuvTexture() {
    av_frame_free(&displayed_yuv_frame_);
    yuv_direct_ = false;
    if (yuv_mapped_) {
        sceGxmUnmapMemory(yuv_pixels_);
        yuv_mapped_ = false;
    }
    yuv_pixels_ = nullptr;
    if (yuv_memblock_ >= 0) {
        sceKernelFreeMemBlock(yuv_memblock_);
        performance::cdram_video -= yuv_memblock_size_;
        yuv_memblock_size_ = 0;
        yuv_memblock_ = -1;
    }
    yuv_valid_ = false;
    yuv_pitch_ = yuv_height_ = 0;
}

bool AnimationPlayer::allocateYuvTexture(int pitch, int aligned_height) {
    const unsigned bytes = pitch * aligned_height * 3 / 2;
    const unsigned allocation = (bytes + 0x3ffffu) & ~0x3ffffu;
    SceKernelAllocMemBlockOpt opt{};
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = 256 * 1024;
    yuv_memblock_ = sceKernelAllocMemBlock("vitagram_yuv", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, allocation, &opt);
    if (yuv_memblock_ >= 0) { yuv_memblock_size_ = allocation; performance::cdram_video += allocation; }
    if (yuv_memblock_ >= 0 && sceKernelGetMemBlockBase(yuv_memblock_, &yuv_pixels_) >= 0 && yuv_pixels_) {
        yuv_mapped_ = sceGxmMapMemory(yuv_pixels_, allocation,
            static_cast<SceGxmMemoryAttribFlags>(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)) >= 0;
    }
    if (yuv_mapped_) {
        std::memset(yuv_pixels_, 0, allocation);
        int rc = sceGxmTextureInitLinear(&yuv_texture_.gxm_tex, yuv_pixels_,
            SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, pitch, aligned_height, 0);
        if (rc >= 0) { yuv_valid_ = true; yuv_pitch_ = pitch; yuv_height_ = aligned_height; }
    }
    if (!yuv_valid_) releaseYuvTexture();
    return yuv_valid_;
}

void AnimationPlayer::copyNv12(const AVFrame* yuv) {
    auto* dest = static_cast<unsigned char*>(yuv_pixels_);
    for (int row = 0; row < yuv->height; ++row)
        std::memcpy(dest + row * yuv_pitch_, yuv->data[0] + row * yuv->linesize[0], yuv->width);
    unsigned char* uv = dest + yuv_pitch_ * yuv_height_;
    for (int row = 0; row < yuv->height / 2; ++row)
        std::memcpy(uv + row * yuv_pitch_, yuv->data[1] + row * yuv->linesize[1], yuv->width);
    vita2d_texture_set_filters(&yuv_texture_, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
}

void AnimationPlayer::detachDisplayedFrame() {
    if (!yuv_valid_ || !yuv_direct_) return;   // an own copy (or nothing) is shown: the pool is already free
    AVFrame* shown = displayed_yuv_frame_;
    displayed_yuv_frame_ = nullptr;
    const int pitch = yuv_pitch_, aligned_height = yuv_height_;
    releaseYuvTexture();   // only forgets the pool surface: direct pictures own no memblock
    if (shown && allocateYuvTexture(pitch, aligned_height)) copyNv12(shown);
    av_frame_free(&shown);   // returns the surface to the decoder pool
}

void AnimationPlayer::setSource(const std::string& path, bool video, bool round, bool prepare, bool cache_only, download_stream::Shared stream) {
    performance::SlowScope slow(performance::SlowPhase::Ui,"animationplayer_setSource");
    pthread_mutex_lock(&mutex_);
    if (path == source_ && video_ == video && round_ == round && prepare_ == prepare && cache_only_ == cache_only && stream_ == stream) { pthread_mutex_unlock(&mutex_); return; }
    source_ = path; stream_ = stream;
    video_ = video; round_ = round; prepare_ = prepare; cache_only_ = cache_only;
    preparation_required_ = false;
    preparation_checked_ = !prepare || !cache_only;
    paused_ = video; finished_ = false; position_us_ = 0; start_us_ = 0; duration_us_ = 0; duration_hint_us_ = 0;
    awaiting_frame_ = video; last_frame_us_ = 0;
    playback_ready_ = false;
    ++generation_;
    ready_ = false;
    av_frame_free(&pending_yuv_frame_);
    pending_direct_ = false;
    yuv_logged_ = false;
    uploaded_frames_ = 0;
    status_ = path.empty() ? "" : prepare && !cache_only ? i18n::text("anim.preparing_video",{"0"}) : video ? i18n::text("anim.play") : i18n::text("anim.preparing_gif");
    pthread_mutex_unlock(&mutex_);
    audio_.setSource(video && !prepare ? path : "", video && !prepare, false, stream);
    if (yuv_valid_) { vita2d_wait_rendering_done(); releaseYuvTexture(); }
    if (texture_) { vita2d_wait_rendering_done(); vita2d_free_texture(texture_); texture_ = nullptr; }
    if (!path.empty() && !started_) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
        int rc = pthread_create(&thread_, &attr, entry, this);
        pthread_attr_destroy(&attr);
        if (!rc) started_ = true;
        else setStatus(generation_, i18n::text("anim.decoder_start_failed"));
    }
}

int64_t AnimationPlayer::seekLimitUs() const {
    const int64_t duration = knownDurationUs();
    if (!stream_) return duration > 0 ? duration : INT64_MAX / 4;
    const auto state = stream_->snapshot();
    if (state.complete) return duration > 0 ? duration : INT64_MAX / 4;
    if (duration <= 0 || state.size <= 0) return 0;
    // MP4 interleaves audio and video by time, so the readable byte share approximates the readable time.
    // Keep a margin for the keyframe search and the audio track, which may sit slightly further in the file.
    const double share = std::min(1.0, static_cast<double>(state.prefix) / static_cast<double>(state.size));
    return std::max<int64_t>(0, static_cast<int64_t>(duration * share) - 5000000);
}

void AnimationPlayer::seekTo(int64_t us) {
    performance::SlowScope slow(performance::SlowPhase::Ui,"animationplayer_seek");
    if (!canSeek()) return;
    const int64_t limit = seekLimitUs();
    pthread_mutex_lock(&mutex_);
    const int64_t target = std::max<int64_t>(0, std::min(us, limit));
    const bool play = finished_ ? true : !paused_.load();   // seeking a finished clip resumes it, like Telegram
    start_us_ = target; position_us_ = target;
    ++generation_; finished_ = false; status_.clear();
    paused_ = !play;
    // The hardware decoder has a fixed pool of frame surfaces; the picture on screen and the queued frame
    // would still hold two of them while the new decoder starts.
    av_frame_free(&pending_yuv_frame_);
    pending_direct_ = false;
    ready_ = false;   // the dropped frame must not fall through to the RGBA upload with an empty buffer
    awaiting_frame_ = true;
    pthread_mutex_unlock(&mutex_);
    // Keep the last picture on screen (as an own copy) under the loading indicator until the new one is decoded.
    if (yuv_valid_) { vita2d_wait_rendering_done(); detachDisplayedFrame(); }
    // Audio restarts at the same point; both clocks then run from the target.
    // Audio starts from the decoder thread once the first picture after the IDR is known, so both begin at the same point
    // (the hardware decoder needs about a second before its first picture, which used to leave the picture behind).
    audio_.setSource("");
    if (round_) {   // software-decoded notes have trustworthy picture timestamps: audio restarts at the target right away
        if (!prepare_) { audio_.setSource(source_, true, false, stream_); audio_.seekTo(target); }
        audio_.setPlaying(play);
    }
}

void AnimationPlayer::toggle() {
    performance::SlowScope slow(performance::SlowPhase::Ui,"animationplayer_toggle");
    if (!video_ || source_.empty() || !started_) return;
    pthread_mutex_lock(&mutex_);
    bool replay = finished_;
    if (replay) { ++generation_; finished_ = false; ready_ = false; position_us_ = 0; start_us_ = 0; status_.clear(); awaiting_frame_ = true; }
    if (replay) { audio_.setSource(""); if (!prepare_) audio_.setSource(source_, true, false, stream_); }
    paused_ = replay ? false : !paused_.load();
    bool play = !paused_;
    if (play) last_frame_us_ = sceKernelGetSystemTimeWide();   // a pause is not a stall
    pthread_mutex_unlock(&mutex_);
    audio_.setPlaying(play);
}

void AnimationPlayer::upload() {
    performance::SlowScope slow(performance::SlowPhase::Ui,"animationplayer_upload");
    if (video_ && finished_) audio_.setPlaying(false);
    pthread_mutex_lock(&mutex_);
    bool pending = ready_;
    pthread_mutex_unlock(&mutex_);
    if (!pending) return;
    vita2d_wait_rendering_done();
    pthread_mutex_lock(&mutex_);
    if (ready_) {
        if (pending_yuv_frame_) {
            if (pending_direct_) {
                releaseYuvTexture();
                displayed_yuv_frame_ = pending_yuv_frame_;
                pending_yuv_frame_ = nullptr;
                pending_direct_ = false;
                AVFrame* displayed = displayed_yuv_frame_;
                const int pitch = (displayed->width + 15) & ~15;
                const int aligned_height = (displayed->height + 15) & ~15;
                int rc = sceGxmTextureInitLinear(&yuv_texture_.gxm_tex, displayed->data[0],
                    SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, pitch, aligned_height, 0);
                if (rc >= 0) {
                    yuv_valid_ = yuv_direct_ = true;
                    yuv_pitch_ = pitch; yuv_height_ = aligned_height;
                    vita2d_texture_set_filters(&yuv_texture_, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
                    ++uploaded_frames_;
                } else {
                    status_ = i18n::text("anim.nv12_failed");
                    av_frame_free(&displayed_yuv_frame_);
                }
                ready_ = false;
                pthread_mutex_unlock(&mutex_);
                return;
            }
            AVFrame* yuv = pending_yuv_frame_;
            const int pitch = (yuv->width + 15) & ~15;
            const int aligned_height = (yuv->height + 15) & ~15;
            if (yuv_valid_ && (pitch != yuv_pitch_ || aligned_height != yuv_height_))
                releaseYuvTexture();
            if (!yuv_valid_ && !allocateYuvTexture(pitch, aligned_height)) status_ = i18n::text("anim.gpu_video");
            if (yuv_valid_) {
                copyNv12(yuv);
                ++uploaded_frames_;
            }
            av_frame_free(&pending_yuv_frame_);
            ready_ = false;
            pthread_mutex_unlock(&mutex_);
            return;
        }
        if (texture_ && (vita2d_texture_get_width(texture_) != (unsigned)width_ ||
                         vita2d_texture_get_height(texture_) != (unsigned)height_)) {
            vita2d_free_texture(texture_); texture_ = nullptr;
        }
        if (width_ <= 0 || height_ <= 0 || pixels_.size() < (size_t)width_ * height_ * 4) {
            ready_ = false;
            pthread_mutex_unlock(&mutex_);
            return;
        }
        if (!texture_) texture_ = vita2d_create_empty_texture(width_, height_);
        if (texture_) {
            auto* dest = static_cast<unsigned char*>(vita2d_texture_get_datap(texture_));
            unsigned stride = vita2d_texture_get_stride(texture_);
            for (int y = 0; y < height_; ++y)
                std::memcpy(dest + y * stride, pixels_.data() + y * width_ * 4, width_ * 4);
            vita2d_texture_set_filters(texture_, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
            ++uploaded_frames_;
        } else status_ = i18n::text("anim.gpu_gif");
        ready_ = false;
    }
    pthread_mutex_unlock(&mutex_);
}

bool AnimationPlayer::buffering() const {
    if (!video_ || source_.empty() || !playback_ready_ || finished_) return false;
    // A paused clip only decodes the preview picture of a seek; at the very start it waits for Play.
    if (awaiting_frame_) return !paused_ || start_us_.load() > 0;
    if (paused_) return false;
    const int64_t last = last_frame_us_.load();
    const int64_t threshold = std::max<int64_t>(700000, 3 * frame_step_us_.load());
    return last > 0 && sceKernelGetSystemTimeWide() - last > threshold;
}

i18n::Text AnimationPlayer::status() {
    pthread_mutex_lock(&mutex_);
    i18n::Text result = status_;
    result.refresh();
    if (video_ && (result.empty() || result == i18n::text("anim.play") || result == i18n::text("anim.video_playing")))
        result = i18n::text(finished_ ? "anim.finished_replay" : paused_ ? "anim.play_pause" : "anim.video_playing_hint");
    pthread_mutex_unlock(&mutex_);
    if (video_ && !audio_.error().empty()) result += " | " + audio_.error();
    return result;
}
void AnimationPlayer::setStatus(unsigned generation, const i18n::Text& text) {
    pthread_mutex_lock(&mutex_);
    if (generation == generation_) status_ = text;
    pthread_mutex_unlock(&mutex_);
}
void AnimationPlayer::shutdown() {
    performance::SlowScope slow(performance::SlowPhase::Ui,"animationplayer_shutdown");
    performance::Scope measurement(performance::Metric::PlayerStop);
    stopping_ = true;
    if (started_) { pthread_join(thread_, nullptr); started_ = false; }
    audio_.shutdown();
    if (texture_) { vita2d_wait_rendering_done(); vita2d_free_texture(texture_); texture_ = nullptr; }
    if (yuv_valid_) { vita2d_wait_rendering_done(); releaseYuvTexture(); }
    av_frame_free(&pending_yuv_frame_);
    for (auto& surface : yuv_surfaces_) {
        if (surface.pixels) sceGxmUnmapMemory(surface.pixels);
        if (surface.uid >= 0) sceKernelFreeMemBlock(surface.uid);
        performance::cdram_video -= surface.allocated;
        surface.pixels = nullptr; surface.uid = -1; surface.allocated = 0;
    }
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
        auto stream = stream_;
        bool video = video_, round = round_, prepare = prepare_, cache_only = cache_only_;
        pthread_mutex_unlock(&mutex_);
        if (generation != handled) {
            handled = generation;
            if (!path.empty()) decode(path, generation, video, round, prepare, cache_only, stream);
        } else sceKernelDelayThread(10000);
    }
}

void AnimationPlayer::decodeTgs(const std::string& path, unsigned generation) {
#ifdef VITAGRAM_RLOTTIE
    std::string packed;
    if (!vault::read_file(path, tgs_policy::kMaxPacked, packed)) { setStatus(generation, i18n::text("anim.tgs_open")); return; }
    std::string data;
    {
        // TGS is gzip; inflate from memory so a sealed file never has to exist in plain text.
        z_stream stream{};
        const bool initialised = inflateInit2(&stream, 16 + MAX_WBITS) == Z_OK;
        bool ok = initialised;
        int status = Z_OK;
        char block[8192];
        stream.next_in = reinterpret_cast<Bytef*>(&packed[0]);
        stream.avail_in = (uInt)packed.size();
        while (ok && status != Z_STREAM_END && !stopping_ && generation == generation_) {
            stream.next_out = reinterpret_cast<Bytef*>(block);
            stream.avail_out = sizeof(block);
            status = inflate(&stream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END) { ok = false; break; }
            data.append(block, sizeof(block) - stream.avail_out);
            if (data.size() > tgs_policy::kMaxJson) break;
        }
        if (initialised) inflateEnd(&stream); // also after a decode error: no zlib state is leaked
        if (stopping_ || generation != generation_) return;
        if (!ok || status != Z_STREAM_END || data.empty() || data.size() > tgs_policy::kMaxJson) {
            setStatus(generation, i18n::text("anim.tgs_invalid")); return;
        }
    }
    // rlottie parses the whole model (and would decode embedded images) before anything below
    // can check it: refuse what stickers never contain first.
    std::string refusal;
    if (!tgs_policy::acceptable(data, &refusal)) {
        std::fprintf(stderr, "TGS refused before parsing: %s\n", refusal.c_str());
        setStatus(generation, i18n::text("anim.tgs_unsupported")); return;
    }
    Lottie_Animation* animation = lottie_animation_from_data(data.c_str(), path.c_str(), "");
    if (!animation) { setStatus(generation, i18n::text("anim.tgs_parse")); return; }
    size_t frames = lottie_animation_get_totalframe(animation);
    double fps = lottie_animation_get_framerate(animation);
    size_t source_w = 0, source_h = 0;
    lottie_animation_get_size(animation, &source_w, &source_h);
    if (!frames || frames > 1800 || !std::isfinite(fps) || fps <= 0 || fps > 120 ||
        !source_w || !source_h || source_w > 4096 || source_h > 4096) {
        lottie_animation_destroy(animation); setStatus(generation, i18n::text("anim.tgs_unsupported")); return;
    }
    double scale = 256.0/std::max(source_w, source_h);
    int w = std::max(1, (int)(source_w*scale)), h = std::max(1, (int)(source_h*scale));
    std::vector<uint32_t> argb(w*h);
    std::vector<unsigned char> rgba(w*h*4);
    int64_t start = sceKernelGetSystemTimeWide();
#if VITAGRAM_TGS_DIAGNOSTIC
    bool frame_saved = false;
#endif
    while (!stopping_ && generation == generation_) {
        int64_t tick = sceKernelGetSystemTimeWide();
        size_t frame = (size_t)((tick-start)*fps/1000000.0) % frames;
        std::fill(argb.begin(), argb.end(), 0);
        lottie_animation_render(animation, frame, argb.data(), w, h, w*4);
#if VITAGRAM_TGS_DIAGNOSTIC
        if (!frame_saved && frame >= std::min<size_t>(20, frames - 1)) {
            frame_saved = true;
            std::fprintf(stderr, "TGS diagnostic frame %zu: %s\n", frame,
                save_tgs_frame(argb, w, h) ? "ux0:data/vitagram/tgs-render-frame.ppm" : "save failed");
        }
#endif
        for (size_t i=0; i<argb.size(); ++i) {
            unsigned pixel = argb[i], alpha = pixel>>24;
            // rlottie outputs premultiplied ARGB; vita2d expects straight RGBA.
            rgba[i*4] = alpha ? std::min(255u, ((pixel>>16)&255)*255/alpha) : 0;
            rgba[i*4+1] = alpha ? std::min(255u, ((pixel>>8)&255)*255/alpha) : 0;
            rgba[i*4+2] = alpha ? std::min(255u, (pixel&255)*255/alpha) : 0;
            rgba[i*4+3] = alpha;
        }
        pthread_mutex_lock(&mutex_);
        if (generation == generation_) { pixels_.swap(rgba); width_ = w; height_ = h; ready_ = true; status_ = i18n::text("anim.tgs_playing"); }
        pthread_mutex_unlock(&mutex_);
        rgba.resize(w*h*4);
        int64_t wait = 33333-(sceKernelGetSystemTimeWide()-tick);
        sceKernelDelayThread(wait > 0 ? (unsigned)wait : 1000);
    }
    lottie_animation_destroy(animation);
#else
    setStatus(generation, i18n::text("anim.tgs_needs_rlottie"));
#endif
}

void AnimationPlayer::decode(const std::string& source_path, unsigned generation, bool video, bool round, bool prepare, bool cache_only, download_stream::Shared download) {
    download_stream::Pin reading{bool(download)};
    std::optional<cpu_scheduler::Media> reservation;
    static std::once_flag logger_once;
    std::call_once(logger_once, [] { av_log_set_callback(capture_ffmpeg_error); });
    logged_codec = nullptr;
    logged_decoder_error_length = 0;
    logged_decoder_error[0] = '\0';
    Job job{this, generation};
    auto early_error = [&](const i18n::Text& text) {
        pthread_mutex_lock(&mutex_);
        if (generation == generation_) { status_ = text; finished_ = true; }
        pthread_mutex_unlock(&mutex_);
    };
    std::string path=source_path;
    if (prepare) {
        reservation.emplace(cpu_scheduler::Role::Video);
        i18n::Text error;
        if (!prepare_portrait_video(source_path,path,
                [&] { return interrupted(&job)!=0; },
                [&](int percent) { setStatus(generation,i18n::text("anim.preparing_video",{std::to_string(percent)})); },
                error, cache_only)) {
            pthread_mutex_lock(&mutex_);
            if (generation == generation_ && cache_only) { preparation_required_ = true; preparation_checked_ = true; }
            pthread_mutex_unlock(&mutex_);
            if (!interrupted(&job)) early_error(cache_only ? i18n::text("video.unsupported_hint") : i18n::Text(error));
            return;
        }
        if (interrupted(&job)) return;
        pthread_mutex_lock(&mutex_);
        if (generation == generation_) preparation_checked_ = true;
        pthread_mutex_unlock(&mutex_);
        audio_.setSource(path,true);
        audio_.setPlaying(!paused_);
        setStatus(generation,i18n::text("anim.play"));
    }
    uLong head_crc = 0, tail_crc = 0;
    int64_t file_size = download ? download->snapshot().size : 0;
    if (!download) {
        vault::Reader file;
        if (!file.open(path)) { early_error(i18n::text("anim.media_open")); return; }
        unsigned char signature[2]{};
        size_t signature_size = file.read(signature, 2) == 2 ? 2 : 0;
        if (!video && signature_size == 2 && signature[0] == 0x1f && signature[1] == 0x8b) {
            file.close(); decodeTgs(path, generation); return;
        }
        int seek_rc = 0;
        file_size = (long)file.size();
        auto sample_crc = [&](long offset, size_t length) {
            uLong crc = crc32(0, Z_NULL, 0);
            if (file.seek(offset, SEEK_SET) < 0) return crc;
            unsigned char block[8192];
            while (length) {
                int64_t count = file.read(block, std::min(length, sizeof(block)));
                if (count <= 0) break;
                crc = crc32(crc, block, (size_t)count);
                length -= (size_t)count;
            }
            return crc;
        };
        if (!seek_rc && file_size > 0) {
            head_crc = sample_crc(0, std::min<long>(file_size, 65536));
            tail_crc = sample_crc(std::max<long>(0, file_size - 65536), std::min<long>(file_size, 65536));
        }
        file.close();
    }
    if (file_size <= 0 || file_size > (video ? 128 : 20) * 1024 * 1024) {
        early_error(video ? i18n::text("anim.limit_video") : i18n::text("anim.limit_gif")); return;
    }
    AVFormatContext* format = avformat_alloc_context();
    VaultAvio sealed_input; // decrypts on the fly when the file is sealed
    DownloadAvio growing_input;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    std::vector<AVFrame*> reordered_frames;
    SwsContext* scaler = nullptr;
    FILE* hardware_log = nullptr;
    bool logged_first_hardware_frame = false;
    unsigned packets_read = 0, frames_decoded = 0;
    auto cleanup = [&]() {
        logged_codec = nullptr;
        if (hardware_log) {
            if (generation == generation_)
                std::fprintf(hardware_log, "summary packets=%u frames=%u uploads=%u audio_clock=%lld\n",
                    packets_read, frames_decoded, uploaded_frames_.load(), (long long)audio_.positionUs());
            std::fclose(hardware_log); hardware_log = nullptr;
        }
        for (AVFrame* queued : reordered_frames) av_frame_free(&queued);
        reordered_frames.clear();
        sws_freeContext(scaler); av_frame_free(&frame); av_packet_free(&packet);
        avcodec_free_context(&codec); avformat_close_input(&format);
        sealed_input.release();
        growing_input.release();
    };
    auto fail = [&](const i18n::Text& message) {
        // A seek that the decoder cannot start from (open GOP, missing parameter sets) degrades to playing from the beginning.
        if (!interrupted(&job) && video && start_us_.load() > 0 && !prepare) {
            pthread_mutex_lock(&mutex_);
            const bool current = generation == generation_;
            if (FILE* fl = diag_log::open(diag_log::Kind::Video, "ux0:data/vitagram/video-seek.log", "a")) {
                std::fprintf(fl, " FALLBACK to start: %s (packets=%u frames=%u seek_base=%lld)\n", message.c_str(),
                    packets_read, frames_decoded, (long long)start_us_.load());
                std::fclose(fl);
            }
            if (current) { start_us_ = 0; position_us_ = 0; ++generation_; finished_ = false; status_.clear(); }
            pthread_mutex_unlock(&mutex_);
            if (current) {
                audio_.setSource("");
                audio_.setSource(source_path, true, false, download);
                audio_.setPlaying(!paused_);
                cleanup();
                return;
            }
        }
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
    if (!format) { fail(i18n::text("anim.memory")); return; }
    format->interrupt_callback = {interrupted, &job};
    format->probesize = 256 * 1024;
    format->max_analyze_duration = 2 * AV_TIME_BASE;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "format_whitelist", media_decode::animation_formats, 0);
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "ignore_loop", "1", 0); // Loop explicitly after draining the decoder.
    // ux0: is a Vita mount, not an FFmpeg protocol.
    const auto stream_state = download ? download->snapshot() : download_stream::State::Snapshot{};
    const std::string input_path = download ? stream_state.path : path;
    const bool growing = download && !(stream_state.complete && vault::is_sealed(input_path));
    const int sealed = growing ? growing_input.attach(format, download) : sealed_input.attach(format, input_path);
    int rc = sealed < 0 ? AVERROR(EIO)
                        : avformat_open_input(&format, sealed ? input_path.c_str() : ("file:" + input_path).c_str(), nullptr, &options);
    av_dict_free(&options);
    // GIF/MOV/WebM headers provide stream parameters; do not decode probe frames before validating dimensions.
    if (rc < 0) { fail(decoder_error("Open", rc)); return; }
    int stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0) { fail(decoder_error("Video stream", stream_index)); return; }
    AVStream* stream = format->streams[stream_index];
    // Without avformat_find_stream_info only the header durations exist; fragmented MP4 has none at file level.
    if (video && format->duration > 0) duration_us_ = format->duration;
    else if (video && stream->duration > 0) duration_us_ = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
    auto* parameters = stream->codecpar;
    if (!media_decode::video_codec(parameters->codec_id, video)) {
        fail(video ? i18n::text("anim.supported_video") : i18n::text("anim.supported_anim")); return;
    }
    auto dimensions_ok = [](int w, int h) { return w > 0 && h > 0 && w <= 1280 && h <= 1280 && (int64_t)w * h <= 1280 * 720; };
    if (!dimensions_ok(parameters->width, parameters->height)) { fail(i18n::text("anim.limit_720p")); return; }
    // The hardware AVC decoder rejects rasters taller than 1088 (e.g. 720x1280 portrait).
    // Telegram metadata can disagree with the stream, so check the real stream here.
    if (parameters->codec_id == AV_CODEC_ID_H264 && parameters->height > 1088) {
        if (video && !round && !prepare) {
            pthread_mutex_lock(&mutex_);
            if (generation == generation_) { preparation_required_ = true; preparation_checked_ = true; }
            pthread_mutex_unlock(&mutex_);
            early_error(i18n::text("video.unsupported_hint"));
        } else {
            early_error(i18n::text("anim.limit_720p"));
        }
        cleanup();
        return;
    }
    if (parameters->codec_id == AV_CODEC_ID_H264) {
        int module_rc = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
        // Some retail Vitas report INVALID_VALUE while the codec engine is
        // already available. The decoder's own initialization is authoritative.
        std::fprintf(stderr, "Animation: AVCDEC module load 0x%08x\n", (unsigned)module_rc);
        hardware_log = diag_log::open(diag_log::Kind::Video, "ux0:data/vitagram/video-hardware.log", "w");
        if (hardware_log) {
            std::fprintf(hardware_log, "VitaGram %s\nbackend=h264_vita\nmodule_load=0x%08x\nprofile=%d level=%d size=%dx%d\nfile_size=%lld head_crc=%08lx tail_crc=%08lx\n",
                VITAGRAM_VERSION, (unsigned)module_rc, parameters->profile, parameters->level,
                parameters->width, parameters->height, (long long)file_size, (unsigned long)head_crc, (unsigned long)tail_crc);
            std::fclose(hardware_log);
            hardware_log = diag_log::open(diag_log::Kind::Video, "ux0:data/vitagram/video-hardware.log", "a");
        }
    }
    const AVCodec* decoder = parameters->codec_id == AV_CODEC_ID_H264
        ? avcodec_find_decoder_by_name("h264_vita") : avcodec_find_decoder(parameters->codec_id);
    codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!codec) { fail(i18n::text("anim.decoder_missing")); return; }
    logged_codec = codec;
    rc = avcodec_parameters_to_context(codec, parameters);
    if (rc < 0) { fail(decoder_error("Parameters", rc)); return; }
    codec->thread_count = 1;
    codec->pkt_timebase = stream->time_base;
    codec->max_pixels = 1280 * 720;
    if (parameters->codec_id == AV_CODEC_ID_H264) codec->pix_fmt = AV_PIX_FMT_NV12;
    const bool direct_video = video && !round && parameters->codec_id == AV_CODEC_ID_H264;
    AVDictionary* codec_options = nullptr;
    if (direct_video) {
        codec->opaque = this;
        codec->get_buffer2 = directBuffer;
        av_dict_set(&codec_options, "vita_h264_dr", "1", 0);
    }
    rc = avcodec_open2(codec, decoder, &codec_options);
    av_dict_free(&codec_options);
    if (rc < 0) { fail(decoder_error("Decoder init", rc)); return; }
    if (hardware_log) {
        std::fprintf(hardware_log, "decoder_open=0 direct=%d pool_capacity=%u\n", direct_video ? 1 : 0,
            static_cast<unsigned>(yuv_surfaces_.size()));
        std::fflush(hardware_log);
    }
    packet = av_packet_alloc(); frame = av_frame_alloc();
    if (!packet || !frame) { fail(i18n::text("anim.gif_memory")); return; }
    AVRational rate = av_guess_frame_rate(format, stream, nullptr);
    int64_t fallback_delay = rate.num > 0 && rate.den > 0 ? av_rescale_q(1, av_inv_q(rate), AVRational{1,1000000}) : 100000;
    fallback_delay = std::max<int64_t>(10000, std::min<int64_t>(1000000, fallback_delay));
    // Without avformat_find_stream_info a progressive MP4 often has no frame
    // rate; the 100 ms default would then pace seeked video at 10 fps while
    // audio runs in real time. Take the real step from the first packet.
    bool frame_step_known = rate.num > 0 && rate.den > 0;
    frame_step_us_ = fallback_delay;
    std::vector<unsigned char> rgba;
    CircleMask circle_mask;
    int64_t origin_pts = AV_NOPTS_VALUE, origin_wall = 0, next_wall = 0, last_output = 0;
    bool had_frame = false;
    i18n::Text frame_error;
    int64_t video_wall = 0, tail_wall = 0, tail_position = 0, video_end = 0;
    int64_t audio_wait_started = 0;
    bool wall_fallback = false;
    bool seek_audio_started = false;
    int64_t seek_key_us = 0;   // timeline position of the IDR picture a seek landed on
    const int64_t timeline_origin_us = format->start_time != AV_NOPTS_VALUE ? format->start_time : 0;
    // After a seek SceVideodec's picture timestamps are not the stream's, and counting pictures at a fixed step
    // drifts on variable-frame-rate clips (phone recordings). Pictures leave the decoder in presentation order,
    // so each one takes the smallest timestamp among the packets sent and not yet presented.
    const int64_t seek_base = video ? start_us_.load() : 0;   // silent clips and audio fallback continue from the seek target
    const bool seek_pts_queue = video && seek_base > 0 && !round && parameters->codec_id == AV_CODEC_ID_H264;
    std::vector<int64_t> seek_pts;
    int64_t last_seek_due = -1;
    // Audio is the master clock. Silent clips use the monotonic wall clock;
    // after a shorter audio track ends, continue its timeline without freezing.
    auto video_time = [&]() -> int64_t {
        int64_t now = sceKernelGetSystemTimeWide();
        if (!video_wall) video_wall = now;
        if (wall_fallback) return seek_base + now - video_wall;
        if (!audio_.finished()) {
            if (!audio_.clockReady()) {
                if (!audio_wait_started) audio_wait_started = now;
                if (now - audio_wait_started >= 2000000) {
                    wall_fallback = true;
                    video_wall = now;
                    if (hardware_log) {
                        std::fprintf(hardware_log, "audio_clock_timeout=1\n");
                        std::fflush(hardware_log);
                    }
                    return seek_base;
                }
                video_wall = now;
                return -1;
            }
            return audio_.positionUs();
        }
        if (audio_.clockReady()) {
            if (!tail_wall) { tail_wall = now; tail_position = audio_.positionUs(); }
            return tail_position + now - tail_wall;
        }
        return seek_base + now - video_wall;
    };
    auto wait_video = [&](int64_t due) -> int64_t {
        while (!interrupted(&job)) {
            // Notes and paused seeks need one preview frame without advancing
            // playback or starting the audio clock.
            if ((round || seek_base > 0) && !had_frame && paused_) return due;
            if (paused_) {
                reservation.reset();
                int64_t start = sceKernelGetSystemTimeWide();
                while (paused_ && !interrupted(&job)) sceKernelDelayThread(10000);
                int64_t delay = sceKernelGetSystemTimeWide() - start;
                if (video_wall) video_wall += delay;
                if (tail_wall) tail_wall += delay;
                if (!interrupted(&job) && video) reservation.emplace(cpu_scheduler::Role::Video);
                continue;
            }
            int64_t time = video_time();
            if (time >= due) return time;
            sceKernelDelayThread(5000);
        }
        return due;
    };
    auto present_frame = [&]() -> bool {
            ++frames_decoded;
            if (hardware_log && !logged_first_hardware_frame) {
                logged_first_hardware_frame = true;
                std::fprintf(hardware_log, "first_frame=1 format=%d pts=%lld best=%lld\n",
                    frame->format, (long long)frame->pts, (long long)frame->best_effort_timestamp);
                std::fflush(hardware_log);
            }
            if (!dimensions_ok(frame->width, frame->height)) {
                frame_error = "Frame size: " + std::to_string(frame->width) + "x" + std::to_string(frame->height);
                return false;
            }
            int64_t presentation_pts = parameters->codec_id == AV_CODEC_ID_H264 && frame->pts != AV_NOPTS_VALUE
                ? frame->pts : frame->best_effort_timestamp;
            int64_t now = sceKernelGetSystemTimeWide();
            if (!origin_wall) { origin_wall = now; origin_pts = presentation_pts; next_wall = now; }
            int64_t due = next_wall;
            if (origin_pts != AV_NOPTS_VALUE && presentation_pts != AV_NOPTS_VALUE) {
                int64_t offset = av_rescale_q(presentation_pts - origin_pts, stream->time_base, AVRational{1,1000000});
                if ((seek_base == 0 || round) && (offset < 0 || offset > (video ? 86400LL : 300LL) * 1000000)) {
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
                if (presentation_pts != AV_NOPTS_VALUE && format->start_time != AV_NOPTS_VALUE)
                    video_due = std::max<int64_t>(0, av_rescale_q(presentation_pts, stream->time_base, AVRational{1,1000000}) - format->start_time);
                // The hardware decoder restarts its picture timestamps after a seek, so count pictures from the IDR instead.
                if (seek_base > 0) {
                    if (seek_pts_queue) {
                        if (!seek_pts.empty()) {
                            video_due = std::max<int64_t>(0, av_rescale_q(seek_pts.front(), stream->time_base, AV_TIME_BASE_Q) - timeline_origin_us);
                            seek_pts.erase(seek_pts.begin());
                        } else video_due = last_seek_due >= 0 ? last_seek_due + fallback_delay : seek_key_us;
                        last_seek_due = video_due;
                    }
                    // Software decoders keep the stream timestamps: the absolute position computed above stays.
                    else if (origin_pts != AV_NOPTS_VALUE && presentation_pts != AV_NOPTS_VALUE)   // notes: pictures restart at 0 too
                        video_due = seek_key_us + std::max<int64_t>(0, av_rescale_q(presentation_pts - origin_pts, stream->time_base, AVRational{1,1000000}));
                }
                video_end = video_due + duration;
                if (media_seek::before_target(video_due, duration, seek_base)) {
                    av_frame_unref(frame); return true;
                }
                if (seek_base > 0 && !round && !seek_audio_started) {
                    seek_audio_started = true;
                    if (!prepare) {
                        audio_.setSource(source_path, true, false, download);
                        audio_.seekTo(video_due);
                        audio_.setPlaying(!paused_);
                    }
                }
                int64_t clock = wait_video(video_due);
                now = sceKernelGetSystemTimeWide();
                if (seek_base > 0 && (frames_decoded <= 3 || frames_decoded % 20 == 0) && frames_decoded <= 140) {
                    if (FILE* fl = diag_log::open(diag_log::Kind::Video, "ux0:data/vitagram/video-seek.log", "a")) {
                        const int64_t decoder_us = presentation_pts != AV_NOPTS_VALUE
                            ? av_rescale_q(presentation_pts, stream->time_base, AV_TIME_BASE_Q) - timeline_origin_us : -1;
                        std::fprintf(fl, "  frame=%u due=%lld clock=%lld lag=%lld decoder=%lld queued=%u packets=%u\n", frames_decoded,
                            (long long)video_due, (long long)clock, (long long)(clock - video_due), (long long)decoder_us,
                            static_cast<unsigned>(seek_pts.size()), packets_read);
                        std::fclose(fl);
                    }
                }
                if (interrupted(&job)) return true;
                pthread_mutex_lock(&mutex_);
                if (generation == generation_) position_us_ = video_due;
                pthread_mutex_unlock(&mutex_);
                // Drop late display frames, never compressed packets or decoder references.
                // A slow decoder can remain behind the audio clock for the
                // whole clip. Refresh the displayed frame periodically even
                // while dropping late frames, otherwise the picture freezes.
                if (had_frame && clock - video_due > 100000 &&
                    last_output && now - last_output < 150000) {
                    av_frame_unref(frame); return true;
                }
            } else {
                while (!interrupted(&job) && (now = sceKernelGetSystemTimeWide()) < due)
                    sceKernelDelayThread((unsigned)std::min<int64_t>(10000, due - now));
            }
            if (interrupted(&job)) return true;
            had_frame = true;
            // Limit conversion and GPU uploads on Vita; the decoder still
            // receives every compressed frame needed for future references.
            const int64_t display_interval = video && !round && frame->format == AV_PIX_FMT_NV12 ? 16000 : 33000;
            if (!last_output || now - last_output >= display_interval) {
                if (video && !round && frame->format == AV_PIX_FMT_NV12 &&
                    !(frame->width & 1) && !(frame->height & 1)) {
                    if (!frame->data[0] || !frame->data[1] ||
                        frame->linesize[0] < frame->width || frame->linesize[1] < frame->width) {
                        frame_error = i18n::text("anim.nv12_layout");
                        return false;
                    }
                    pthread_mutex_lock(&mutex_);
                    if (generation == generation_) {
                        if (!pending_yuv_frame_) pending_yuv_frame_ = av_frame_alloc();
                        if (!pending_yuv_frame_) {
                            pthread_mutex_unlock(&mutex_);
                            frame_error = i18n::text("anim.frame_memory");
                            return false;
                        }
                        av_frame_unref(pending_yuv_frame_);
                        if (av_frame_ref(pending_yuv_frame_, frame) < 0) {
                            pthread_mutex_unlock(&mutex_);
                            frame_error = i18n::text("anim.frame_retain");
                            return false;
                        }
                        width_ = frame->width; height_ = frame->height;
                        pending_direct_ = direct_video;
                        ready_ = true; status_ = i18n::text("anim.video_playing");
                        awaiting_frame_ = false; last_frame_us_ = now;
                    }
                    pthread_mutex_unlock(&mutex_);
                    last_output = now;
                    av_frame_unref(frame);
                    return true;
                }
                float max_width = video ? 480.0f : 640.0f;
                float max_height = video ? 270.0f : 360.0f;
                float scale = std::min(1.0f, std::min(max_width / frame->width, max_height / frame->height));
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
                    circle_mask.resize(w, h);
                    circle_mask.apply(rgba.data());
                }
                pthread_mutex_lock(&mutex_);
                if (generation == generation_) {
                    pixels_.swap(rgba); width_ = w; height_ = h; ready_ = true; status_ = i18n::text(video ? "anim.video_playing" : "anim.play_pause");
                    awaiting_frame_ = false; last_frame_us_ = now;
                }
                pthread_mutex_unlock(&mutex_);
                last_output = now;
            }
            av_frame_unref(frame);
            return true;
    };
    // SceVideodec may return a future reference frame before its B-frames.
    // Hold a small decode-order window and present it by the raw picture PTS.
    auto drain_reordered = [&](bool all) -> bool {
        while (!reordered_frames.empty() && (all || reordered_frames.size() > 5)) {
            AVFrame* queued = reordered_frames.front();
            reordered_frames.erase(reordered_frames.begin());
            av_frame_move_ref(frame, queued);
            av_frame_free(&queued);
            if (!present_frame()) return false;
        }
        return true;
    };
    // Called after each accepted packet, including the null drain packet.
    auto receive = [&]() -> bool {
        frame_error.clear();
        while (!interrupted(&job)) {
            int result = avcodec_receive_frame(codec, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
            if (result < 0) { frame_error = decoder_error("Receive frame", result); return false; }
            if (video && parameters->codec_id == AV_CODEC_ID_H264) {
                AVFrame* queued = av_frame_clone(frame);
                av_frame_unref(frame);
                if (!queued) { frame_error = "Not enough memory for video reorder queue"; return false; }
                int64_t pts = queued->pts != AV_NOPTS_VALUE ? queued->pts : queued->best_effort_timestamp;
                auto at = std::upper_bound(reordered_frames.begin(), reordered_frames.end(), pts,
                    [](int64_t timestamp, const AVFrame* item) {
                        int64_t item_pts = item->pts != AV_NOPTS_VALUE ? item->pts : item->best_effort_timestamp;
                        return timestamp < item_pts;
                    });
                reordered_frames.insert(at, queued);
                if (!drain_reordered(false)) return false;
            } else if (!present_frame()) return false;
        }
        return true;
    };
    if (video && seek_base > 0) {
        // The hardware decoder only outputs pictures from an IDR picture on; started at any other sync sample it
        // buffers access units without output until "ES buffer full" (0x8062000a). So land on a real IDR packet:
        // seek, inspect the first packet, and step back to an earlier keyframe while it is not an IDR.
        const bool h264 = parameters->codec_id == AV_CODEC_ID_H264;
        const int length_size = parameters->extradata_size > 4 && parameters->extradata[0] == 1 ? (parameters->extradata[4] & 3) + 1 : 4;
        auto is_idr = [&](const AVPacket* candidate) {
            const uint8_t* data = candidate->data; const int size = candidate->size;
            if (size >= 4 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1))) {   // Annex B
                for (int i = 0; i + 4 < size; ++i)
                    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1f) == 5) return true;
                return false;
            }
            for (int i = 0; i + length_size < size;) {   // length-prefixed (MP4)
                int nal = 0;
                for (int k = 0; k < length_size; ++k) nal = (nal << 8) | data[i + k];
                i += length_size;
                if (nal <= 0 || i + nal > size) break;
                if ((data[i] & 0x1f) == 5) return true;
                i += nal;
            }
            return false;
        };
        const int64_t first_ts = timeline_origin_us;
        int64_t target = av_rescale_q(seek_base + first_ts, AV_TIME_BASE_Q, stream->time_base);
        const int64_t floor_ts = av_rescale_q(first_ts, AV_TIME_BASE_Q, stream->time_base);
        AVPacket* probe = av_packet_alloc();
        bool landed = false;
        FILE* seek_log = diag_log::open(diag_log::Kind::Video, "ux0:data/vitagram/video-seek.log", "a");
        if (seek_log) std::fprintf(seek_log, "VitaGram %s seek target_us=%lld h264=%d streaming=%d tb=%d/%d\n", VITAGRAM_VERSION,
            (long long)seek_base, h264 ? 1 : 0, growing ? 1 : 0, stream->time_base.num, stream->time_base.den);
        for (int attempt = 0; probe && attempt < 16 && !interrupted(&job); ++attempt) {
            if (av_seek_frame(format, stream_index, target, AVSEEK_FLAG_BACKWARD) < 0) break;
            bool got = false;
            while (av_read_frame(format, probe) >= 0) {
                if (probe->stream_index == stream_index) { got = true; break; }
                av_packet_unref(probe);
            }
            if (!got) break;
            const int64_t key = probe->dts != AV_NOPTS_VALUE ? probe->dts : probe->pts;
            const bool idr = !h264 || is_idr(probe);
            if (seek_log) std::fprintf(seek_log, " attempt=%d key_ts=%lld size=%d idr=%d\n", attempt, (long long)key, probe->size, idr ? 1 : 0);
            av_packet_unref(probe);
            if (idr) {
                // Position the demuxer on that IDR packet again so the decoder receives it first.
                landed = key != AV_NOPTS_VALUE && av_seek_frame(format, stream_index, key, AVSEEK_FLAG_BACKWARD) >= 0;
                if (landed) seek_key_us = std::max<int64_t>(0, av_rescale_q(key, stream->time_base, AV_TIME_BASE_Q) - first_ts);
                break;
            }
            if (key == AV_NOPTS_VALUE || key <= floor_ts) break;
            target = key - 1;
        }
        av_packet_free(&probe);
        if (seek_log) { std::fprintf(seek_log, " landed=%d key_us=%lld\n", landed ? 1 : 0, (long long)seek_key_us); std::fclose(seek_log); }
        if (!landed && !interrupted(&job)) { fail("Seek: no IDR picture before the target"); return; }
    }
    // Only now are the headers, codec and seek position usable. A partial
    // download may have blocked in avformat_open_input while looking for moov.
    pthread_mutex_lock(&mutex_);
    if (generation == generation_ && video) playback_ready_ = true;
    pthread_mutex_unlock(&mutex_);
    bool hardware_recovered = false;
    while (!interrupted(&job)) {
        if (video && paused_ && !((round || seek_base > 0) && !had_frame)) {
            reservation.reset();
            while (paused_ && !interrupted(&job)) sceKernelDelayThread(10000);
            if (interrupted(&job)) break;
        }
        if (video && !reservation) reservation.emplace(cpu_scheduler::Role::Video);
        // The hardware decoder swallows packets faster than it outputs pictures; when late frames are dropped nothing
        // paces the feed and its elementary stream buffer overflows ("Send packet" failure). Keep the pipeline shallow.
        if (video && direct_video) {
            for (int waited = 0; waited < 400 && !interrupted(&job) && packets_read > frames_decoded + 250; ++waited) {
                sceKernelDelayThread(5000);
                if (!receive()) { fail(frame_error); return; }
            }
        }
        rc = av_read_frame(format, packet);
        if (rc == AVERROR_EOF) {
            rc = avcodec_send_packet(codec, nullptr);
            if (rc < 0) { fail(decoder_error("Send EOF", rc)); return; }
            if (!receive()) { fail(frame_error); return; }
            if (!drain_reordered(true)) { fail(frame_error); return; }
            if (hardware_log) {
                std::fprintf(hardware_log, "eof=1 packets=%u frames=%u\n", packets_read, frames_decoded);
                std::fflush(hardware_log);
            }
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
            if (!frame_step_known && packet->duration > 0) {
                frame_step_known = true;
                fallback_delay = std::max<int64_t>(10000, std::min<int64_t>(1000000,
                    av_rescale_q(packet->duration, stream->time_base, AVRational{1,1000000})));
                frame_step_us_ = fallback_delay;
                if (hardware_log) {
                    std::fprintf(hardware_log, "frame_step_us=%lld source=packet\n", (long long)fallback_delay);
                    std::fflush(hardware_log);
                }
            }
            rc = avcodec_send_packet(codec, packet);
            if (rc < 0) {
                // A failed access unit can leave SceVideodec's state unusable.
                // Keep hardware decoding, but restart once at a later keyframe.
                if (direct_video && !hardware_recovered && packet->dts != AV_NOPTS_VALUE) {
                    hardware_recovered = true;
                    int64_t target = packet->dts + std::max<int64_t>(1, packet->duration);
                    if (hardware_log) {
                        std::fprintf(hardware_log, "decode_error=%d packet=%u dts=%lld detail=%s\n",
                            rc, packets_read, (long long)packet->dts, logged_decoder_error);
                        std::fflush(hardware_log);
                    }
                    av_packet_unref(packet);
                    int seek_result = av_seek_frame(format, stream_index, target, 0);
                    if (hardware_log) {
                        std::fprintf(hardware_log, "recovery_seek=%d target=%lld\n", seek_result, (long long)target);
                        std::fflush(hardware_log);
                    }
                    if (seek_result >= 0) {
                        for (AVFrame* queued : reordered_frames) av_frame_free(&queued);
                        reordered_frames.clear();
                        seek_pts.clear();
                        av_frame_unref(frame);
                        avcodec_free_context(&codec);
                        codec = avcodec_alloc_context3(decoder);
                        if (codec && avcodec_parameters_to_context(codec, parameters) >= 0) {
                            logged_codec = codec;
                            codec->thread_count = 1;
                            codec->pkt_timebase = stream->time_base;
                            codec->max_pixels = 1280 * 720;
                            codec->pix_fmt = AV_PIX_FMT_NV12;
                            codec->opaque = this;
                            codec->get_buffer2 = directBuffer;
                            AVDictionary* recovery_options = nullptr;
                            av_dict_set(&recovery_options, "vita_h264_dr", "1", 0);
                            int open_result = avcodec_open2(codec, decoder, &recovery_options);
                            av_dict_free(&recovery_options);
                            if (hardware_log) {
                                std::fprintf(hardware_log, "recovery_open=%d\n", open_result);
                                std::fflush(hardware_log);
                            }
                            if (open_result >= 0) { logged_decoder_error_length = 0; logged_decoder_error[0] = '\0'; continue; }
                        }
                    }
                }
                fail(decoder_error("Send packet", rc)); return;
            }
            if (seek_pts_queue && packet->pts != AV_NOPTS_VALUE && seek_pts.size() < 1024)
                seek_pts.insert(std::upper_bound(seek_pts.begin(), seek_pts.end(), packet->pts), packet->pts);
            if (!receive()) { fail(frame_error); return; }
            av_packet_unref(packet);
        } else av_packet_unref(packet);
    }
    cleanup();
}
