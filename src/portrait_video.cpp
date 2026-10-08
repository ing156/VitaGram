#include "portrait_video.h"
#include "i18n.h"
#include "minih264e.h"
#include "vault.h"
#include "vault_avio.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace {
struct Resources {
    AVFormatContext* input = nullptr;
    AVFormatContext* output = nullptr;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = nullptr;
    AVPacket* coded = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* scaled = nullptr;
    SwsContext* scaler = nullptr;
    void* persist = nullptr;
    void* scratch = nullptr;
    std::vector<AVPacket*> pending_audio;
    VaultAvio sealed_input; // released after the demuxer is closed (members die after the body)
    ~Resources() {
        for (auto* p : pending_audio) av_packet_free(&p);
        if (output) {
            if (output->pb) avio_closep(&output->pb);
            avformat_free_context(output);
        }
        avformat_close_input(&input);
        avcodec_free_context(&decoder);
        av_packet_free(&packet); av_packet_free(&coded);
        av_frame_free(&frame); av_frame_free(&scaled);
        sws_freeContext(scaler);
        std::free(persist); std::free(scratch);
    }
};

std::vector<uint8_t> h264_configuration(const uint8_t* bytes, int size) {
    std::vector<uint8_t> result;
    auto prefix = [&](int i) { return i+3<size && !bytes[i] && !bytes[i+1] &&
        (bytes[i+2]==1 || (!bytes[i+2] && bytes[i+3]==1)); };
    for (int i=0; i+4<size;) {
        if (!prefix(i)) { ++i; continue; }
        int begin=i, header=i+(bytes[i+2]==1 ? 3 : 4); i=header+1;
        while (i<size && !prefix(i)) ++i;
        int type=bytes[header]&31;
        if (type==7 || type==8) result.insert(result.end(),bytes+begin,bytes+i);
    }
    return result;
}

uint64_t cache_key(const std::string& path, long size, FILE* file) {
    uint64_t hash=1469598103934665603ULL;
    for (unsigned char c : path) { hash^=c; hash*=1099511628211ULL; }
    for (unsigned i=0;i<sizeof(size);++i) { hash^=(unsigned char)(size>>(i*8)); hash*=1099511628211ULL; }
    unsigned char sample[4096];
    for (long offset : {0L,std::max(0L,size-(long)sizeof(sample))}) {
        if (std::fseek(file,offset,SEEK_SET)) continue;
        size_t count=std::fread(sample,1,sizeof(sample),file);
        for (size_t i=0;i<count;++i) { hash^=sample[i]; hash*=1099511628211ULL; }
    }
    return hash;
}
}

bool prepare_portrait_video(const std::string& source, std::string& prepared,
                            const std::function<bool()>& cancelled,
                            const std::function<void(int)>& progress,
                            std::string& error) {
    // minih264e requires both dimensions to be multiples of 16.
    constexpr int Width=432, Height=768;
    prepared.clear(); error.clear();
    FILE* file=std::fopen(source.c_str(),"rb");
    if (!file) { error=tr("pv.open_failed"); return false; }
    std::fseek(file,0,SEEK_END); long input_size=std::ftell(file);
    if (input_size<=0) { std::fclose(file); error=tr("pv.empty"); return false; }
    uint64_t key=cache_key(source,input_size,file);
    std::fclose(file);
    const int64_t progress_size=std::max<int64_t>(1,vault::plain_size(source)); // plain bytes, as the demuxer counts them
    char name[96];
    std::snprintf(name,sizeof(name),"ux0:data/vitagram/portrait-432x768-%016llx.mp4",
                  (unsigned long long)key);
    prepared=name;
    file=std::fopen(prepared.c_str(),"rb");
    if (file) {
        std::fseek(file,0,SEEK_END); long ready_size=std::ftell(file); std::fclose(file);
        if (ready_size>1024) { progress(100); return true; }
    }
    std::string partial=prepared+".part";
    FILE* diagnostic=std::fopen("ux0:data/vitagram/portrait-video.log","w");
    if (diagnostic) {
        std::fprintf(diagnostic,"VitaGram %s input=%ld output=432x768 cache=%016llx\n",
                     VITAGRAM_VERSION,input_size,(unsigned long long)key);
        std::fflush(diagnostic);
    }
    Resources r;
    auto fail=[&](const std::string& message, int rc=0) {
        error=message;
        if (rc<0) { char detail[AV_ERROR_MAX_STRING_SIZE]; av_strerror(rc,detail,sizeof(detail)); error+=std::string(": ")+detail; }
        if (diagnostic) { std::fprintf(diagnostic,"error=%s rc=%d\n",message.c_str(),rc); std::fflush(diagnostic); }
        prepared.clear(); return false;
    };
    uint64_t output_bytes=0;
    auto work=[&]() -> bool {
        r.input=avformat_alloc_context();
        if (!r.input) return fail(tr("pv.read_failed"),AVERROR(ENOMEM));
        const int sealed=r.sealed_input.attach(r.input,source); // sealed sources are decrypted on the fly
        int rc=sealed<0 ? AVERROR(EIO) : avformat_open_input(&r.input,sealed ? source.c_str() : ("file:"+source).c_str(),nullptr,nullptr);
        if (rc<0) return fail(tr("pv.read_failed"),rc);
        r.input->interrupt_callback={[](void* opaque)->int { return (*static_cast<const std::function<bool()>*>(opaque))(); },
                                      (void*)&cancelled};
        rc=avformat_find_stream_info(r.input,nullptr);
        if (rc<0) return fail(tr("pv.streams_failed"),rc);
        int vi=av_find_best_stream(r.input,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
        if (vi<0 || r.input->streams[vi]->codecpar->codec_id!=AV_CODEC_ID_H264)
            return fail(tr("pv.h264_only"));
        int ai=av_find_best_stream(r.input,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
        if (ai>=0 && r.input->streams[ai]->codecpar->codec_id!=AV_CODEC_ID_AAC)
            return fail(tr("pv.aac_needed"));
        const AVCodec* software=avcodec_find_decoder_by_name("h264");
        r.decoder=software ? avcodec_alloc_context3(software) : nullptr;
        if (!r.decoder) return fail(tr("pv.sw_decoder_missing"));
        r.decoder->thread_count=1;
        if ((rc=avcodec_parameters_to_context(r.decoder,r.input->streams[vi]->codecpar))<0 ||
            (rc=avcodec_open2(r.decoder,software,nullptr))<0) return fail(tr("pv.h264_open"),rc);
        H264E_create_param_t params{};
        params.width=Width; params.height=Height; params.gop=30;
        params.const_input_flag=1; params.vbv_size_bytes=256000; params.enableNEON=1;
        int persistent=0,scratch=0;
        if (H264E_sizeof(&params,&persistent,&scratch)) return fail(tr("pv.h264_size"));
        r.persist=memalign(64,persistent); r.scratch=memalign(64,scratch);
        if (!r.persist || !r.scratch || H264E_init((H264E_persist_t*)r.persist,&params))
            return fail(tr("pv.prep_memory"));
        if ((rc=avformat_alloc_output_context2(&r.output,nullptr,"mp4",partial.c_str()))<0 || !r.output)
            return fail(tr("vne.mp4_muxer_missing_short"),rc);
        AVStream* out_video=avformat_new_stream(r.output,nullptr);
        AVStream* out_audio=ai>=0 ? avformat_new_stream(r.output,nullptr) : nullptr;
        if (!out_video || (ai>=0 && !out_audio)) return fail(tr("pv.mp4_streams"));
        out_video->time_base=r.input->streams[vi]->time_base;
        out_video->codecpar->codec_type=AVMEDIA_TYPE_VIDEO;
        out_video->codecpar->codec_id=AV_CODEC_ID_H264;
        out_video->codecpar->width=Width; out_video->codecpar->height=Height;
        out_video->codecpar->format=AV_PIX_FMT_YUV420P;
        if (out_audio) {
            if ((rc=avcodec_parameters_copy(out_audio->codecpar,r.input->streams[ai]->codecpar))<0)
                return fail(tr("pv.aac_copy"),rc);
            out_audio->codecpar->codec_tag=0;
            out_audio->time_base=r.input->streams[ai]->time_base;
        }
        r.packet=av_packet_alloc(); r.coded=av_packet_alloc();
        r.frame=av_frame_alloc(); r.scaled=av_frame_alloc();
        if (!r.packet || !r.coded || !r.frame || !r.scaled) return fail(tr("pv.frames_memory"));
        r.scaled->format=AV_PIX_FMT_YUV420P; r.scaled->width=Width; r.scaled->height=Height;
        if ((rc=av_frame_get_buffer(r.scaled,32))<0) return fail(tr("pv.frame_alloc"),rc);
        bool header=false;
        int64_t last_pts=AV_NOPTS_VALUE;
        int frame_count=0;
        auto write_audio=[&](AVPacket* p)->bool {
            output_bytes+=p->size;
            if (output_bytes>120ULL*1024*1024) return false;
            av_packet_rescale_ts(p,r.input->streams[ai]->time_base,out_audio->time_base);
            p->stream_index=out_audio->index;
            int result=av_interleaved_write_frame(r.output,p);
            return result>=0;
        };
        auto emit=[&]() -> bool {
            r.scaler=sws_getCachedContext(r.scaler,r.frame->width,r.frame->height,
                (AVPixelFormat)r.frame->format,Width,Height,AV_PIX_FMT_YUV420P,SWS_BILINEAR,
                nullptr,nullptr,nullptr);
            if (!r.scaler || av_frame_make_writable(r.scaled)<0) return fail(tr("pv.scale_failed"));
            if (sws_scale(r.scaler,r.frame->data,r.frame->linesize,0,r.frame->height,
                          r.scaled->data,r.scaled->linesize)<=0) return fail(tr("pv.scale_error"));
            H264E_io_yuv_t yuv{};
            for (int i=0;i<3;++i) { yuv.yuv[i]=r.scaled->data[i]; yuv.stride[i]=r.scaled->linesize[i]; }
            H264E_run_param_t run{}; run.encode_speed=H264E_SPEED_FASTEST;
            run.desired_frame_bytes=12000; run.qp_min=20; run.qp_max=45;
            unsigned char* bytes=nullptr; int length=0;
            if (H264E_encode((H264E_persist_t*)r.persist,(H264E_scratch_t*)r.scratch,
                             &run,&yuv,&bytes,&length) || length<=0) return fail(tr("vne.h264_encode"));
            if (!header) {
                auto config=h264_configuration(bytes,length);
                if (config.empty()) return fail(tr("pv.no_h264_params"));
                auto* extra=(uint8_t*)av_mallocz(config.size()+AV_INPUT_BUFFER_PADDING_SIZE);
                if (!extra) return fail(tr("pv.mp4_memory"));
                std::memcpy(extra,config.data(),config.size());
                out_video->codecpar->extradata=extra;
                out_video->codecpar->extradata_size=config.size();
                if ((rc=avio_open(&r.output->pb,("file:"+partial).c_str(),AVIO_FLAG_WRITE))<0 ||
                    (rc=avformat_write_header(r.output,nullptr))<0) return fail(tr("vne.mp4_create"),rc);
                header=true;
                for (auto* p:r.pending_audio) if (!write_audio(p)) return fail(tr("pv.aac_write"));
                for (auto*& p:r.pending_audio) av_packet_free(&p);
                r.pending_audio.clear();
            }
            if ((rc=av_new_packet(r.coded,length))<0) return fail(tr("pv.h264_memory"),rc);
            output_bytes+=length;
            if (output_bytes>120ULL*1024*1024) return fail(tr("pv.limit"));
            std::memcpy(r.coded->data,bytes,length);
            int64_t pts=r.frame->best_effort_timestamp;
            if (pts==AV_NOPTS_VALUE || (last_pts!=AV_NOPTS_VALUE && pts<=last_pts))
                pts=last_pts==AV_NOPTS_VALUE ? 0 : last_pts+1;
            r.coded->pts=r.coded->dts=pts;
            r.coded->duration=last_pts==AV_NOPTS_VALUE ? 1 : std::max<int64_t>(1,pts-last_pts);
            if (frame_count%params.gop==0) r.coded->flags|=AV_PKT_FLAG_KEY;
            last_pts=pts; ++frame_count;
            av_packet_rescale_ts(r.coded,r.input->streams[vi]->time_base,out_video->time_base);
            r.coded->stream_index=out_video->index;
            rc=av_interleaved_write_frame(r.output,r.coded);
            av_packet_unref(r.coded);
            return rc>=0 || fail(tr("pv.mp4_write"),rc);
        };
        auto drain=[&]() -> bool {
            for (;;) {
                int result=avcodec_receive_frame(r.decoder,r.frame);
                if (result==AVERROR(EAGAIN) || result==AVERROR_EOF) return true;
                if (result<0) return fail(tr("pv.decode_error"),result);
                bool ok=emit(); av_frame_unref(r.frame);
                if (!ok) return false;
            }
        };
        while (!cancelled() && (rc=av_read_frame(r.input,r.packet))>=0) {
            if (r.packet->stream_index==vi) {
                rc=avcodec_send_packet(r.decoder,r.packet);
                if (rc<0 || !drain()) return error.empty() ? fail(tr("pv.h264_error"),rc) : false;
            } else if (r.packet->stream_index==ai) {
                if (header) {
                    if (!write_audio(r.packet)) return fail(tr("pv.aac_write"));
                } else {
                    AVPacket* copy=av_packet_clone(r.packet);
                    if (!copy) return fail(tr("pv.aac_memory"));
                    r.pending_audio.push_back(copy);
                }
            }
            av_packet_unref(r.packet);
            if (progress_size>0 && (frame_count%16)==0 && r.input->pb)
                progress(std::min(99,(int)(avio_tell(r.input->pb)*100/progress_size)));
        }
        if (cancelled()) return fail(tr("pv.cancelled"));
        if (rc!=AVERROR_EOF && rc<0) return fail(tr("pv.read_error"),rc);
        if ((rc=avcodec_send_packet(r.decoder,nullptr))<0 || !drain())
            return error.empty() ? fail(tr("pv.h264_finish"),rc) : false;
        if (!header || (rc=av_write_trailer(r.output))<0 || (rc=avio_closep(&r.output->pb))<0)
            return fail(tr("pv.mp4_finish"),rc);
        return true;
    };
    bool ok=work();
    if (!ok) {
        if (r.output && r.output->pb) avio_closep(&r.output->pb);
        std::remove(partial.c_str());
        if (diagnostic) std::fclose(diagnostic);
        return false;
    }
    if (std::rename(partial.c_str(),prepared.c_str())!=0) {
        fail(tr("pv.mp4_save"));
        std::remove(partial.c_str());
        if (diagnostic) std::fclose(diagnostic);
        return false;
    }
    if (vault::sealing() && vault::seal_file(prepared)==vault::Convert::Failed) {
        // The result is only a cache of the user's video: never leave it behind in plain text.
        fail(tr("pv.mp4_save"));
        std::remove(prepared.c_str());
        if (diagnostic) std::fclose(diagnostic);
        return false;
    }
    if (diagnostic) {
        std::fprintf(diagnostic,"complete=1 bytes=%llu\n",(unsigned long long)std::max<int64_t>(0,output_bytes));
        std::fclose(diagnostic);
    }
    progress(100); return true;
}
