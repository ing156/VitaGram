#include "media_decode_policy.h"
#include "webm_alpha.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#include <cassert>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>
static int open(AVFormatContext** format,const char* path,const char* whitelist) {
    *format=avformat_alloc_context(); assert(*format);
    (*format)->probesize=256*1024;
    (*format)->max_analyze_duration=2*AV_TIME_BASE;
    AVDictionary* options=nullptr;
    av_dict_set(&options,"format_whitelist",whitelist,0);
    av_dict_set(&options,"protocol_whitelist","file",0);
    av_dict_set(&options,"ignore_loop","1",0);
    int rc=avformat_open_input(format,(std::string("file:")+path).c_str(),nullptr,&options);
    av_dict_free(&options); return rc;
}
int main(int argc,char** argv) {
    assert(argc==2);
    AVFormatContext* format=nullptr;
    // Reproduce the exact reported error with the old player options.
    int old=open(&format,argv[1],"gif,mov,mp4,m4a,3gp,3g2,mj2");
    assert(old==AVERROR(EINVAL)); avformat_close_input(&format);
    std::puts("Old whitelist: reproduced Open: -22 Invalid argument");
    assert(!open(&format,argv[1],media_decode::animation_formats));
    int stream=av_find_best_stream(format,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0); assert(stream>=0);
    auto* params=format->streams[stream]->codecpar;
    assert(params->codec_id==AV_CODEC_ID_VP8 || params->codec_id==AV_CODEC_ID_VP9);
    assert(media_decode::video_codec(params->codec_id,false) && media_decode::video_codec(params->codec_id,true));
    assert(!media_decode::video_codec(AV_CODEC_ID_HEVC,true));
    assert(!media_decode::video_codec(AV_CODEC_ID_AV1,false));
    assert(params->width>0 && params->height>0 && params->width<=1280 && params->height<=1280 && int64_t(params->width)*params->height<=1280*720);
    const AVCodec* decoder=avcodec_find_decoder(params->codec_id); assert(decoder);
    AVCodecContext* codec=avcodec_alloc_context3(decoder); assert(codec);
    assert(!avcodec_parameters_to_context(codec,params));
    codec->thread_count=1; codec->max_pixels=1280*720;
    codec->pkt_timebase=format->streams[stream]->time_base;
    assert(!avcodec_open2(codec,decoder,nullptr));
    AVPacket* packet=av_packet_alloc(); AVFrame* frame=av_frame_alloc(); assert(packet && frame);
    WebmAlpha alpha_decoder;
    // Reject malformed alpha payloads before handing them to the decoder.
    for (int size : {7, 8, 9}) {
        AVPacket* invalid=av_packet_alloc(); assert(invalid);
        uint8_t* side=av_packet_new_side_data(invalid,AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL,size); assert(side);
        std::memset(side,0,size); side[size>=8 ? 7 : 6]=1;
        WebmAlpha bad;
        assert(bad.send(invalid,codec)<0);
        av_packet_free(&invalid);
    }
    SwsContext* scaler=nullptr; int frames=0; bool nonzero=false, transparent=false, opaque=false, partial=false;
    int64_t first_pts=AV_NOPTS_VALUE, last_pts=AV_NOPTS_VALUE;
    auto drain=[&]() {
        for (;;) {
            int rc=avcodec_receive_frame(codec,frame);
            if (rc==AVERROR(EAGAIN) || rc==AVERROR_EOF) break;
            assert(rc>=0);
            assert(!alpha_decoder.attach(frame));
            if (frame->best_effort_timestamp!=AV_NOPTS_VALUE) {
                if(first_pts==AV_NOPTS_VALUE) first_pts=frame->best_effort_timestamp;
                assert(last_pts==AV_NOPTS_VALUE || frame->best_effort_timestamp>=last_pts);
                last_pts=frame->best_effort_timestamp;
            }
            float scale=std::min(1.0f,std::min(640.0f/frame->width,360.0f/frame->height));
            int w=std::max(1,int(frame->width*scale)), h=std::max(1,int(frame->height*scale));
            scaler=sws_getCachedContext(scaler,frame->width,frame->height,(AVPixelFormat)frame->format,
                w,h,AV_PIX_FMT_RGBA,SWS_FAST_BILINEAR | SWS_ACCURATE_RND,nullptr,nullptr,nullptr); assert(scaler);
            std::vector<uint8_t> rgba(w*h*4);
            uint8_t* dst[]={rgba.data()}; int strides[]={w*4};
            assert(sws_scale(scaler,frame->data,frame->linesize,0,frame->height,dst,strides)==h);
            for(size_t i=0;i<rgba.size();i+=4) {
                nonzero=nonzero || rgba[i] || rgba[i+1] || rgba[i+2];
                transparent=transparent || rgba[i+3]==0;
                opaque=opaque || rgba[i+3]==255;
                partial=partial || (rgba[i+3]>0 && rgba[i+3]<255);
            }
            ++frames; av_frame_unref(frame);
        }
    };
    int read;
    while ((read=av_read_frame(format,packet))>=0) {
        if(packet->stream_index==stream) { assert(!alpha_decoder.send(packet,codec)); assert(!avcodec_send_packet(codec,packet)); drain(); }
        av_packet_unref(packet);
    }
    assert(read==AVERROR_EOF); assert(!avcodec_send_packet(codec,nullptr)); drain();
    assert(frames>1 && nonzero);
    auto* alpha=av_dict_get(format->streams[stream]->metadata,"alpha_mode",nullptr,0);
    if (alpha && std::string(alpha->value)=="1") assert(transparent && opaque && partial);
    else assert(!transparent && !partial && opaque);
    double span=(last_pts-first_pts)*av_q2d(format->streams[stream]->time_base);
    std::printf("WebM/VP9: %dx%d, %d frames, frame timestamp span %.3f seconds, alpha_mode=%s; decode and RGBA conversion passed\n",
        params->width,params->height,frames,span,alpha ? alpha->value : "absent");
    assert(first_pts!=AV_NOPTS_VALUE && span>0);
    int total=frames;
    int64_t start=format->streams[stream]->start_time;
    assert(!av_seek_frame(format,stream,start==AV_NOPTS_VALUE ? 0 : start,AVSEEK_FLAG_BACKWARD));
    avcodec_flush_buffers(codec); alpha_decoder.flush(); first_pts=last_pts=AV_NOPTS_VALUE;
    while(av_read_frame(format,packet)>=0) {
        if(packet->stream_index==stream) { assert(!alpha_decoder.send(packet,codec)); assert(!avcodec_send_packet(codec,packet)); drain(); }
        av_packet_unref(packet);
    }
    assert(!avcodec_send_packet(codec,nullptr)); drain();
    assert(frames==total*2); std::puts("Sticker loop seek and all frames after rewind passed");
    sws_freeContext(scaler); av_packet_free(&packet); av_frame_free(&frame);
    avcodec_free_context(&codec); avformat_close_input(&format);
    // The parallel audio reader must accept the same container even for a silent video.
    assert(!open(&format,argv[1],media_decode::video_formats)); avformat_close_input(&format);
}
