#include "test_language.h"
#include "video_note_encoder.h"
#include "video_note_frame.h"
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
int main(int argc,char** argv) {
    use_test_language();
    assert(argc==2);
    using namespace video_note;
    std::string base=std::string(argv[1])+"/note", raw=base+".frames", pcm=base+".pcm", mp4=base+".mp4", error;
    FILE* f=std::fopen(raw.c_str(),"wb"); assert(f);
    std::vector<uint8_t> camera(320*240*3/2,128), square(FrameBytes);
    // Include an irregular frame interval, as occurs when switching cameras.
    std::vector<uint32_t> timestamps(90);
    for (unsigned i=0;i<timestamps.size();++i) timestamps[i]=i*SampleRate/FrameRate+(i>=45 ? 4000 : 0);
    timestamps[30]=timestamps[29]+1;
    constexpr unsigned samples=52000;
    for (uint32_t pts : timestamps) {
        std::fill(camera.begin(),camera.begin()+320*240,100+pts/400);
        crop(camera.data(),square.data());
        assert(std::fwrite(&pts,4,1,f)==1 && std::fwrite(square.data(),1,FrameBytes,f)==FrameBytes);
    }
    std::fclose(f);
    f=std::fopen(pcm.c_str(),"wb"); assert(f);
    for (unsigned i=0;i<samples;++i) { int16_t v=std::sin(i*6.283185307*440/SampleRate)*10000; assert(std::fwrite(&v,2,1,f)==1); }
    std::fclose(f);
    std::atomic<bool> cancel{false}; std::atomic<int> progress{0};
    bool ok=encode_video_note(raw,pcm,mp4,samples,cancel,progress,error);
    if (!ok) std::cerr << error << '\n';
    assert(ok && error.empty() && progress==100);
    AVFormatContext* input=nullptr;
    assert(avformat_open_input(&input,mp4.c_str(),nullptr,nullptr)>=0);
    assert(avformat_find_stream_info(input,nullptr)>=0);
    assert(input->nb_streams==2 && input->duration>=3200000 && input->duration<=3350000);
    AVCodecContext* decoders[2]{}; int video=-1,audio=-1;
    for (unsigned i=0;i<input->nb_streams;++i) {
        auto* par=input->streams[i]->codecpar;
        if (par->codec_type==AVMEDIA_TYPE_VIDEO) {
            video=i; assert(par->codec_id==AV_CODEC_ID_H264 && par->width==Side && par->height==Side);
        } else { audio=i; assert(par->codec_id==AV_CODEC_ID_AAC && par->ch_layout.nb_channels==1); }
        auto* codec=avcodec_find_decoder(par->codec_id); assert(codec);
        decoders[i]=avcodec_alloc_context3(codec); assert(decoders[i]);
        assert(avcodec_parameters_to_context(decoders[i],par)>=0 && avcodec_open2(decoders[i],codec,nullptr)>=0);
    }
    assert(video>=0 && audio>=0);
    AVPacket* packet=av_packet_alloc(); AVFrame* frame=av_frame_alloc();
    int pictures=0,audio_samples=0; unsigned video_packets=0; bool sound=false; int64_t last[2]={INT64_MIN,INT64_MIN};
    auto receive=[&](int index) {
        int rc;
        while ((rc=avcodec_receive_frame(decoders[index],frame))>=0) {
            if (index==video) {
                ++pictures;
                assert(frame->data[0][0]<35 && frame->data[0][120*frame->linesize[0]+120]>70);
            } else {
                audio_samples+=frame->nb_samples;
                assert(frame->format==AV_SAMPLE_FMT_FLTP);
                auto* samples=reinterpret_cast<float*>(frame->data[0]);
                for (int i=0;i<frame->nb_samples;++i) if (std::abs(samples[i])>0.01) sound=true;
            }
            av_frame_unref(frame);
        }
        assert(rc==AVERROR(EAGAIN) || rc==AVERROR_EOF);
    };
    while (av_read_frame(input,packet)>=0) {
        int i=packet->stream_index;
        assert(packet->dts>last[i]); last[i]=packet->dts;
        if (i==video) {
            assert(video_packets<timestamps.size());
            assert(packet->pts==av_rescale_q(timestamps[video_packets],AVRational{1,SampleRate},input->streams[i]->time_base));
            assert(bool(packet->flags&AV_PKT_FLAG_KEY)==(video_packets%KeyframeInterval==0)); ++video_packets;
        }
        assert(avcodec_send_packet(decoders[i],packet)>=0); receive(i); av_packet_unref(packet);
    }
    for (int i=0;i<2;++i) { assert(avcodec_send_packet(decoders[i],nullptr)>=0); receive(i); avcodec_free_context(&decoders[i]); }
    assert(pictures==90 && video_packets==90 && audio_samples>=51000 && audio_samples<=54000 && sound);
    av_packet_free(&packet); av_frame_free(&frame); avformat_close_input(&input);
    cancel=true;
    assert(!encode_video_note(raw,pcm,base+"-cancel.mp4",32000,cancel,progress,error));
    assert(!std::fopen((base+"-cancel.mp4").c_str(),"rb"));
    cancel=false;
    assert(!encode_video_note(raw,pcm,base+"-bad.mp4",0,cancel,progress,error));
    // A truncated frame must never leave a sendable MP4 behind.
    f=std::fopen(raw.c_str(),"wb"); uint32_t zero=0; std::fwrite(&zero,4,1,f); std::fclose(f);
    assert(!encode_video_note(raw,pcm,base+"-bad.mp4",32000,cancel,progress,error));
    assert(!std::fopen((base+"-bad.mp4").c_str(),"rb"));
    std::cout << "MP4/H.264/AAC decode, circle, duration, irregular timestamps, cancel and truncated input: PASS\n";
}
