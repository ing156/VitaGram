#include "video_note_encoder.h"
#include "i18n.h"
#include "video_note_frame.h"
#include "minih264e.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <malloc.h>
#include <memory>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace {
constexpr int VideoBitrate = 1536000;
constexpr int AudioBitrate = 32000;
constexpr int VideoFrameBudget = VideoBitrate / (8 * video_note::FrameRate);
struct Resources {
    FILE *frames = nullptr, *pcm = nullptr;
    void *persist = nullptr, *scratch = nullptr, *pixels = nullptr;
    AVFormatContext* format = nullptr;
    AVCodecContext* audio = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    ~Resources() {
        if (frames) std::fclose(frames);
        if (pcm) std::fclose(pcm);
        if (format) { if (format->pb) avio_closep(&format->pb); avformat_free_context(format); }
        avcodec_free_context(&audio); av_packet_free(&packet); av_frame_free(&frame);
        std::free(persist); std::free(scratch); std::free(pixels);
    }
};
// Extract Annex B SPS/PPS only; libavformat writes the MP4 avcC configuration.
std::vector<uint8_t> configuration(const uint8_t* data, int size) {
    std::vector<uint8_t> result;
    auto prefix = [&](int i) { return i+3 < size && !data[i] && !data[i+1] &&
        (data[i+2] == 1 || (!data[i+2] && data[i+3] == 1)); };
    for (int i=0; i+4<size;) {
        if (!prefix(i)) { ++i; continue; }
        int begin=i, header=i+(data[i+2] == 1 ? 3 : 4); i=header+1;
        while (i<size && !prefix(i)) ++i;
        int type=data[header]&31;
        if (type==7 || type==8) result.insert(result.end(), data+begin, data+i);
    }
    return result;
}
}

bool encode_video_note(const std::string& frames, const std::string& pcm, const std::string& output,
                       uint32_t samples, const std::atomic<bool>& cancel, std::atomic<int>& progress,
                       std::string& error) {
    using namespace video_note;
    Resources r;
    bool success = false;
    error.clear(); progress = 0;
    auto fail = [&](const std::string& what, int rc = 0) {
        error = what;
        if (rc < 0) { char text[AV_ERROR_MAX_STRING_SIZE]; av_strerror(rc, text, sizeof(text)); error += std::string(": ")+text; }
        return false;
    };
    auto work = [&]() -> bool {
        if (!samples || samples > SampleRate*MaxSeconds) return fail(tr("vne.bad_duration"));
        r.frames = std::fopen(frames.c_str(), "rb"); r.pcm = std::fopen(pcm.c_str(), "rb");
        if (!r.frames || !r.pcm) return fail(tr("vne.open_failed"));
        H264E_create_param_t params{};
        params.width = params.height = Side; params.gop = KeyframeInterval; params.const_input_flag = 1;
        params.vbv_size_bytes = VideoBitrate / 8 * 2; params.enableNEON = 1;
        int persistent=0, scratch=0;
        if (H264E_sizeof(&params, &persistent, &scratch)) return fail(tr("vne.h264_params"));
        r.persist=memalign(64,persistent); r.scratch=memalign(64,scratch); r.pixels=memalign(64,FrameBytes);
        if (!r.persist || !r.scratch || !r.pixels) return fail(tr("vne.h264_memory"));
        auto* encoder=static_cast<H264E_persist_t*>(r.persist);
        if (H264E_init(encoder,&params)) return fail(tr("vne.h264_open"));
        const AVCodec* aac=avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!aac || !(r.audio=avcodec_alloc_context3(aac))) return fail(tr("vne.aac_missing"));
        int rc=avformat_alloc_output_context2(&r.format,nullptr,"mp4",output.c_str());
        if (rc<0 || !r.format) return fail(tr("vne.mp4_muxer_missing"),rc);
        r.audio->sample_rate=SampleRate; r.audio->sample_fmt=AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&r.audio->ch_layout,1);
        r.audio->bit_rate=AudioBitrate; r.audio->time_base={1,SampleRate};
        r.audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((rc=avcodec_open2(r.audio,aac,nullptr))<0) return fail("AAC encoder",rc);
        AVStream* video=avformat_new_stream(r.format,nullptr);
        AVStream* audio=avformat_new_stream(r.format,nullptr);
        if (!video || !audio) return fail(tr("vne.streams"));
        video->time_base={1,SampleRate}; video->avg_frame_rate={FrameRate,1};
        video->codecpar->codec_type=AVMEDIA_TYPE_VIDEO; video->codecpar->codec_id=AV_CODEC_ID_H264;
        video->codecpar->width=video->codecpar->height=Side; video->codecpar->format=AV_PIX_FMT_YUV420P;
        audio->time_base=r.audio->time_base;
        if ((rc=avcodec_parameters_from_context(audio->codecpar,r.audio))<0) return fail("AAC parameters",rc);
        r.frame=av_frame_alloc(); r.packet=av_packet_alloc();
        if (!r.frame || !r.packet) return fail(tr("vne.mp4_memory"));
        r.frame->format=r.audio->sample_fmt; r.frame->sample_rate=SampleRate;
        av_channel_layout_default(&r.frame->ch_layout,1); r.frame->nb_samples=r.audio->frame_size;
        if ((rc=av_frame_get_buffer(r.frame,0))<0) return fail("AAC buffer",rc);
        std::vector<int16_t> pcm_samples(r.audio->frame_size);
        auto drain = [&]() -> bool {
            for (;;) {
                int result=avcodec_receive_packet(r.audio,r.packet);
                if (result==AVERROR(EAGAIN) || result==AVERROR_EOF) return true;
                if (result<0) return fail("AAC packet",result);
                av_packet_rescale_ts(r.packet,r.audio->time_base,audio->time_base);
                r.packet->stream_index=audio->index;
                result=av_interleaved_write_frame(r.format,r.packet); av_packet_unref(r.packet);
                if (result<0) return fail("MP4 audio",result);
            }
        };
        uint32_t audio_pts=0;
        auto audio_until = [&](uint32_t until) -> bool {
            while (audio_pts<until && audio_pts<samples && !cancel) {
                int count=std::min<uint32_t>(r.audio->frame_size,samples-audio_pts);
                if (std::fread(pcm_samples.data(),sizeof(int16_t),count,r.pcm)!=(size_t)count) return fail(tr("vne.audio_read"));
                if ((rc=av_frame_make_writable(r.frame))<0) return fail("AAC writable frame",rc);
                auto* dst=reinterpret_cast<float*>(r.frame->data[0]);
                for (int i=0;i<count;++i) dst[i]=pcm_samples[i]/32768.0f;
                r.frame->nb_samples=count; r.frame->pts=audio_pts; audio_pts+=count;
                if ((rc=avcodec_send_frame(r.audio,r.frame))<0 || !drain()) return error.empty() ? fail("AAC frame",rc) : false;
            }
            return true;
        };
        uint32_t pts=0;
        if (std::fread(&pts,sizeof(pts),1,r.frames)!=1 || pts!=0) return fail(tr("vne.no_first_frame"));
        int frame_number=0;
        bool header=false;
        while (!cancel) {
            if (pts>=samples || std::fread(r.pixels,1,FrameBytes,r.frames)!=FrameBytes) return fail(tr("vne.frame_corrupt"));
            uint32_t next=samples;
            size_t got=std::fread(&next,1,sizeof(next),r.frames);
            if (std::ferror(r.frames) || (got && got!=sizeof(next)) || next<=pts || next>samples)
                return fail(tr("vne.timestamps_corrupt"));
            H264E_io_yuv_t input{};
            auto* pixels=static_cast<uint8_t*>(r.pixels);
            input.yuv[0]=pixels; input.yuv[1]=pixels+Side*Side; input.yuv[2]=pixels+Side*Side*5/4;
            input.stride[0]=Side; input.stride[1]=input.stride[2]=Side/2;
            H264E_run_param_t run{}; run.encode_speed=H264E_SPEED_FASTEST;
            // Restore 6400 bytes per image at 30 FPS. Audio PTS are quantized;
            // their interval must not starve an image or inflate a camera gap.
            run.desired_frame_bytes=VideoFrameBudget;
            run.qp_min=20; run.qp_max=45;
            unsigned char* coded=nullptr; int size=0;
            if (H264E_encode(encoder,static_cast<H264E_scratch_t*>(r.scratch),&run,&input,&coded,&size) || size<=0)
                return fail(tr("vne.h264_encode"));
            if (!header) {
                auto config=configuration(coded,size);
                if (config.empty()) return fail(tr("vne.no_sps_pps"));
                video->codecpar->extradata=static_cast<uint8_t*>(av_mallocz(config.size()+AV_INPUT_BUFFER_PADDING_SIZE));
                if (!video->codecpar->extradata) return fail(tr("vne.mp4_header_memory"));
                std::memcpy(video->codecpar->extradata,config.data(),config.size());
                video->codecpar->extradata_size=config.size();
                if ((rc=avio_open(&r.format->pb,("file:"+output).c_str(),AVIO_FLAG_WRITE))<0) return fail(tr("vne.mp4_create"),rc);
                if ((rc=avformat_write_header(r.format,nullptr))<0) return fail("MP4 header",rc);
                header=true;
            }
            if ((rc=av_new_packet(r.packet,size))<0) return fail("H.264 packet",rc);
            std::memcpy(r.packet->data,coded,size);
            r.packet->pts=r.packet->dts=pts; r.packet->duration=next-pts;
            if (frame_number%KeyframeInterval==0) r.packet->flags |= AV_PKT_FLAG_KEY;
            av_packet_rescale_ts(r.packet,{1,SampleRate},video->time_base); r.packet->stream_index=video->index;
            rc=av_interleaved_write_frame(r.format,r.packet); av_packet_unref(r.packet);
            if (rc<0) return fail("MP4 video",rc);
            if (!audio_until(next)) return false;
            progress=std::min(99,(int)(next*100/samples)); ++frame_number;
            if (!got) break;
            pts=next;
        }
        if (cancel) return fail(tr("vne.cancelled"));
        if (!header || !audio_until(samples)) return false;
        if ((rc=avcodec_send_frame(r.audio,nullptr))<0 || !drain()) return error.empty() ? fail("AAC flush",rc) : false;
        if ((rc=av_write_trailer(r.format))<0) return fail("MP4 trailer",rc);
        if ((rc=avio_closep(&r.format->pb))<0) return fail("MP4 close",rc);
        progress=100; return true;
    };
    success=work();
    if (!success) {
        if (r.format && r.format->pb) avio_closep(&r.format->pb);
        std::remove(output.c_str());
    }
    return success;
}
