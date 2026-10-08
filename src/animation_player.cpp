#include "performance.h"
#include "i18n.h"
#include "animation_player.h"
#include "media_decode_policy.h"
#include "portrait_video.h"
#include "vault.h"
#include "vault_avio.h"
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
    FILE* log = std::fopen("ux0:data/vitagram/animation-error.log", "w");
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
                    FILE* log = std::fopen("ux0:data/vitagram/video-hardware.log", "a");
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
    FILE* log = std::fopen("ux0:data/vitagram/video-hardware.log", "a");
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

void AnimationPlayer::setSource(const std::string& path, bool video, bool round, bool prepare) {
    performance::SlowScope slow(performance::SlowPhase::Ui,"animationplayer_setSource");
    pthread_mutex_lock(&mutex_);
    if (path == source_ && video_ == video && round_ == round && prepare_ == prepare) { pthread_mutex_unlock(&mutex_); return; }
    source_ = path;
    video_ = video; round_ = round; prepare_ = prepare;
    paused_ = video; finished_ = false; position_us_ = 0;
    ++generation_;
    ready_ = false;
    av_frame_free(&pending_yuv_frame_);
    pending_direct_ = false;
    yuv_logged_ = false;
    uploaded_frames_ = 0;
    status_ = path.empty() ? "" : prepare ? tr("anim.preparing_video",{"0"}) : video ? tr("anim.play") : tr("anim.preparing_gif");
    pthread_mutex_unlock(&mutex_);
    audio_.setSource(video && !prepare ? path : "", video && !prepare);
    if (yuv_valid_) { vita2d_wait_rendering_done(); releaseYuvTexture(); }
    if (texture_) { vita2d_wait_rendering_done(); vita2d_free_texture(texture_); texture_ = nullptr; }
    if (!path.empty() && !started_) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
        int rc = pthread_create(&thread_, &attr, entry, this);
        pthread_attr_destroy(&attr);
        if (!rc) started_ = true;
        else setStatus(generation_, tr("anim.decoder_start_failed"));
    }
}

void AnimationPlayer::toggle() {
    performance::SlowScope slow(performance::SlowPhase::Ui,"animationplayer_toggle");
    if (!video_ || source_.empty() || !started_) return;
    pthread_mutex_lock(&mutex_);
    bool replay = finished_;
    if (replay) { ++generation_; finished_ = false; ready_ = false; position_us_ = 0; status_.clear(); }
    if (replay) { audio_.setSource(""); if (!prepare_) audio_.setSource(source_, true); }
    paused_ = replay ? false : !paused_.load();
    bool play = !paused_;
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
                    status_ = tr("anim.nv12_failed");
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
            if (!yuv_valid_) {
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
                if (!yuv_valid_) { releaseYuvTexture(); status_ = tr("anim.gpu_video"); }
            }
            if (yuv_valid_) {
                auto* dest = static_cast<unsigned char*>(yuv_pixels_);
                for (int row = 0; row < yuv->height; ++row)
                    std::memcpy(dest + row * pitch, yuv->data[0] + row * yuv->linesize[0], yuv->width);
                unsigned char* uv = dest + pitch * aligned_height;
                for (int row = 0; row < yuv->height / 2; ++row)
                    std::memcpy(uv + row * pitch, yuv->data[1] + row * yuv->linesize[1], yuv->width);
                vita2d_texture_set_filters(&yuv_texture_, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
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
        if (!texture_) texture_ = vita2d_create_empty_texture(width_, height_);
        if (texture_) {
            auto* dest = static_cast<unsigned char*>(vita2d_texture_get_datap(texture_));
            unsigned stride = vita2d_texture_get_stride(texture_);
            for (int y = 0; y < height_; ++y)
                std::memcpy(dest + y * stride, pixels_.data() + y * width_ * 4, width_ * 4);
            vita2d_texture_set_filters(texture_, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
            ++uploaded_frames_;
        } else status_ = tr("anim.gpu_gif");
        ready_ = false;
    }
    pthread_mutex_unlock(&mutex_);
}

std::string AnimationPlayer::status() {
    pthread_mutex_lock(&mutex_);
    std::string result = status_;
    if (video_ && (result.empty() || result == tr("anim.play") || result == tr("anim.video_playing")))
        result = tr(finished_ ? "anim.finished_replay" : paused_ ? "anim.play_pause" : "anim.video_playing_hint");
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
        bool video = video_, round = round_, prepare = prepare_;
        pthread_mutex_unlock(&mutex_);
        if (generation != handled) {
            handled = generation;
            if (!path.empty()) decode(path, generation, video, round, prepare);
        } else sceKernelDelayThread(10000);
    }
}

void AnimationPlayer::decodeTgs(const std::string& path, unsigned generation) {
#ifdef VITAGRAM_RLOTTIE
    std::string packed;
    if (!vault::read_file(path, 2*1024*1024, packed)) { setStatus(generation, tr("anim.tgs_open")); return; }
    std::string data;
    {
        // TGS is gzip; inflate from memory so a sealed file never has to exist in plain text.
        z_stream stream{};
        bool ok = inflateInit2(&stream, 16 + MAX_WBITS) == Z_OK;
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
            if (data.size() > 2*1024*1024) break;
        }
        if (ok) inflateEnd(&stream);
        if (stopping_ || generation != generation_) return;
        if (!ok || status != Z_STREAM_END || data.empty() || data.size() > 2*1024*1024) {
            setStatus(generation, tr("anim.tgs_invalid")); return;
        }
    }
    Lottie_Animation* animation = lottie_animation_from_data(data.c_str(), path.c_str(), "");
    if (!animation) { setStatus(generation, tr("anim.tgs_parse")); return; }
    size_t frames = lottie_animation_get_totalframe(animation);
    double fps = lottie_animation_get_framerate(animation);
    size_t source_w = 0, source_h = 0;
    lottie_animation_get_size(animation, &source_w, &source_h);
    if (!frames || frames > 1800 || !std::isfinite(fps) || fps <= 0 || fps > 120 ||
        !source_w || !source_h || source_w > 4096 || source_h > 4096) {
        lottie_animation_destroy(animation); setStatus(generation, tr("anim.tgs_unsupported")); return;
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
        if (generation == generation_) { pixels_.swap(rgba); width_ = w; height_ = h; ready_ = true; status_ = tr("anim.tgs_playing"); }
        pthread_mutex_unlock(&mutex_);
        rgba.resize(w*h*4);
        int64_t wait = 33333-(sceKernelGetSystemTimeWide()-tick);
        sceKernelDelayThread(wait > 0 ? (unsigned)wait : 1000);
    }
    lottie_animation_destroy(animation);
#else
    setStatus(generation, tr("anim.tgs_needs_rlottie"));
#endif
}

void AnimationPlayer::decode(const std::string& source_path, unsigned generation, bool video, bool round, bool prepare) {
    std::optional<cpu_scheduler::Media> reservation;
    static std::once_flag logger_once;
    std::call_once(logger_once, [] { av_log_set_callback(capture_ffmpeg_error); });
    logged_codec = nullptr;
    logged_decoder_error_length = 0;
    logged_decoder_error[0] = '\0';
    Job job{this, generation};
    auto early_error = [&](const std::string& text) {
        pthread_mutex_lock(&mutex_);
        if (generation == generation_) { status_ = text; finished_ = true; }
        pthread_mutex_unlock(&mutex_);
    };
    std::string path=source_path;
    if (prepare) {
        reservation.emplace(cpu_scheduler::Role::Video);
        std::string error;
        if (!prepare_portrait_video(source_path,path,
                [&] { return interrupted(&job)!=0; },
                [&](int percent) { setStatus(generation,tr("anim.preparing_video",{std::to_string(percent)})); },
                error)) {
            if (!interrupted(&job)) early_error(error);
            return;
        }
        if (interrupted(&job)) return;
        audio_.setSource(path,true);
        audio_.setPlaying(!paused_);
        setStatus(generation,tr("anim.play"));
    }
    vault::Reader file;
    if (!file.open(path)) { early_error(tr("anim.media_open")); return; }
    unsigned char signature[2]{};
    size_t signature_size = file.read(signature, 2) == 2 ? 2 : 0;
    if (!video && signature_size == 2 && signature[0] == 0x1f && signature[1] == 0x8b) {
        file.close(); decodeTgs(path, generation); return;
    }
    int seek_rc = 0;
    long file_size = (long)file.size();
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
    uLong head_crc = 0, tail_crc = 0;
    if (!seek_rc && file_size > 0) {
        head_crc = sample_crc(0, std::min<long>(file_size, 65536));
        tail_crc = sample_crc(std::max<long>(0, file_size - 65536), std::min<long>(file_size, 65536));
    }
    file.close();
    if (seek_rc || file_size <= 0 || file_size > (video ? 128 : 20) * 1024 * 1024) {
        early_error(video ? tr("anim.limit_video") : tr("anim.limit_gif")); return;
    }
    AVFormatContext* format = avformat_alloc_context();
    VaultAvio sealed_input; // decrypts on the fly when the file is sealed
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
    if (!format) { fail(tr("anim.memory")); return; }
    format->interrupt_callback = {interrupted, &job};
    format->probesize = 256 * 1024;
    format->max_analyze_duration = 2 * AV_TIME_BASE;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "format_whitelist", media_decode::animation_formats, 0);
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "ignore_loop", "1", 0); // Loop explicitly after draining the decoder.
    // ux0: is a Vita mount, not an FFmpeg protocol.
    const int sealed = sealed_input.attach(format, path);
    int rc = sealed < 0 ? AVERROR(EIO)
                        : avformat_open_input(&format, sealed ? path.c_str() : ("file:" + path).c_str(), nullptr, &options);
    av_dict_free(&options);
    // GIF/MOV/WebM headers provide stream parameters; do not decode probe frames before validating dimensions.
    if (rc < 0) { fail(decoder_error("Open", rc)); return; }
    int stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0) { fail(decoder_error("Video stream", stream_index)); return; }
    AVStream* stream = format->streams[stream_index];
    auto* parameters = stream->codecpar;
    if (!media_decode::video_codec(parameters->codec_id, video)) {
        fail(video ? tr("anim.supported_video") : tr("anim.supported_anim")); return;
    }
    auto dimensions_ok = [](int w, int h) { return w > 0 && h > 0 && w <= 1280 && h <= 1280 && (int64_t)w * h <= 1280 * 720; };
    if (!dimensions_ok(parameters->width, parameters->height)) { fail(tr("anim.limit_720p")); return; }
    if (parameters->codec_id == AV_CODEC_ID_H264) {
        int module_rc = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
        // Some retail Vitas report INVALID_VALUE while the codec engine is
        // already available. The decoder's own initialization is authoritative.
        std::fprintf(stderr, "Animation: AVCDEC module load 0x%08x\n", (unsigned)module_rc);
        hardware_log = std::fopen("ux0:data/vitagram/video-hardware.log", "w");
        if (hardware_log) {
            std::fprintf(hardware_log, "VitaGram %s\nbackend=h264_vita\nmodule_load=0x%08x\nprofile=%d level=%d size=%dx%d\nfile_size=%ld head_crc=%08lx tail_crc=%08lx\n",
                VITAGRAM_VERSION, (unsigned)module_rc, parameters->profile, parameters->level,
                parameters->width, parameters->height, file_size, (unsigned long)head_crc, (unsigned long)tail_crc);
            if (parameters->height > 1088)
                std::fprintf(hardware_log, "portrait_hardware_probe=1 coded_height=%d\n", parameters->height);
            std::fclose(hardware_log);
            hardware_log = std::fopen("ux0:data/vitagram/video-hardware.log", "a");
        }
    }
    const AVCodec* decoder = parameters->codec_id == AV_CODEC_ID_H264
        ? avcodec_find_decoder_by_name("h264_vita") : avcodec_find_decoder(parameters->codec_id);
    codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!codec) { fail(tr("anim.decoder_missing")); return; }
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
    if (!packet || !frame) { fail(tr("anim.gif_memory")); return; }
    AVRational rate = av_guess_frame_rate(format, stream, nullptr);
    int64_t fallback_delay = rate.num > 0 && rate.den > 0 ? av_rescale_q(1, av_inv_q(rate), AVRational{1,1000000}) : 100000;
    fallback_delay = std::max<int64_t>(10000, std::min<int64_t>(1000000, fallback_delay));
    std::vector<unsigned char> rgba;
    int64_t origin_pts = AV_NOPTS_VALUE, origin_wall = 0, next_wall = 0, last_output = 0;
    bool had_frame = false;
    std::string frame_error;
    int64_t video_wall = 0, tail_wall = 0, tail_position = 0, video_end = 0;
    int64_t audio_wait_started = 0;
    bool wall_fallback = false;
    // Audio is the master clock. Silent clips use the monotonic wall clock;
    // after a shorter audio track ends, continue its timeline without freezing.
    auto video_time = [&]() -> int64_t {
        int64_t now = sceKernelGetSystemTimeWide();
        if (!video_wall) video_wall = now;
        if (wall_fallback) return now - video_wall;
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
                    return 0;
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
        return now - video_wall;
    };
    auto wait_video = [&](int64_t due) -> int64_t {
        while (!interrupted(&job)) {
            // A video note starts paused, but it still needs one decoded,
            // circular frame as its preview before the user presses Play.
            if (round && !had_frame && paused_) return due;
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
                if (presentation_pts != AV_NOPTS_VALUE && format->start_time != AV_NOPTS_VALUE)
                    video_due = std::max<int64_t>(0, av_rescale_q(presentation_pts, stream->time_base, AVRational{1,1000000}) - format->start_time);
                video_end = video_due + duration;
                int64_t clock = wait_video(video_due);
                now = sceKernelGetSystemTimeWide();
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
                        frame_error = tr("anim.nv12_layout");
                        return false;
                    }
                    pthread_mutex_lock(&mutex_);
                    if (generation == generation_) {
                        if (!pending_yuv_frame_) pending_yuv_frame_ = av_frame_alloc();
                        if (!pending_yuv_frame_) {
                            pthread_mutex_unlock(&mutex_);
                            frame_error = tr("anim.frame_memory");
                            return false;
                        }
                        av_frame_unref(pending_yuv_frame_);
                        if (av_frame_ref(pending_yuv_frame_, frame) < 0) {
                            pthread_mutex_unlock(&mutex_);
                            frame_error = tr("anim.frame_retain");
                            return false;
                        }
                        width_ = frame->width; height_ = frame->height;
                        pending_direct_ = direct_video;
                        ready_ = true; status_ = tr("anim.video_playing");
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
                    // Video notes are square; crop their displayed pixels to a circle.
                    float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
                    float radius = std::min(w, h) * 0.5f;
                    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                        if ((x-cx)*(x-cx) + (y-cy)*(y-cy) > radius*radius)
                            rgba[(y*w+x)*4+3] = 0;
                }
                pthread_mutex_lock(&mutex_);
                if (generation == generation_) {
                    pixels_.swap(rgba); width_ = w; height_ = h; ready_ = true; status_ = video ? tr("anim.video_playing") : "GIF - playing";
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
    bool hardware_recovered = false;
    while (!interrupted(&job)) {
        if (video && paused_ && !(round && !had_frame)) {
            reservation.reset();
            while (paused_ && !interrupted(&job)) sceKernelDelayThread(10000);
            if (interrupted(&job)) break;
        }
        if (video && !reservation) reservation.emplace(cpu_scheduler::Role::Video);
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
            if (!receive()) { fail(frame_error); return; }
            av_packet_unref(packet);
        } else av_packet_unref(packet);
    }
    cleanup();
}
