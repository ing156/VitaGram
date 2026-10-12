#include "portrait_video.h"
#include "video_note_encoder.h"
#include "video_note_frame.h"
#include "cpu_scheduler.h"
#include "vault.h"
#include "test_language.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace cpu_scheduler {
Thread::Thread(Role) : id_(0) {}
Thread::~Thread() = default;
}

static std::string contents(const std::string& path) {
    std::ifstream stream(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
    assert(argc==2 && chdir(argv[1])==0);
    use_test_language();
    assert(mkdir("ux0:data",0700)==0 && mkdir("ux0:data/vitagram",0700)==0);
    vault::set_config_path("vault.json"); vault::load();
    std::vector<uint8_t> frame(video_note::FrameBytes,128);
    FILE* raw=std::fopen("frames","wb"), *pcm=std::fopen("pcm","wb");
    assert(raw && pcm);
    constexpr uint32_t samples=8*16000;
    for (uint32_t i=0;i<240;++i) {
        const uint32_t pts=i*16000/30;
        std::fill(frame.begin(),frame.begin()+240*240,80+i%60);
        assert(std::fwrite(&pts,4,1,raw)==1 && std::fwrite(frame.data(),1,frame.size(),raw)==frame.size());
    }
    std::vector<int16_t> silence(samples);
    assert(std::fwrite(silence.data(),2,samples,pcm)==samples);
    std::fclose(raw); std::fclose(pcm);
    std::atomic<bool> cancel{false}; std::atomic<int> percent{0};
    std::string error, prepared;
    assert(encode_video_note("frames","pcm","source.mp4",samples,cancel,percent,error));
    auto cancelled=[] { return false; };
    auto progress=[&](int value) { percent=value; };
    // Opening without a cached copy must not silently start conversion.
    assert(!prepare_portrait_video("source.mp4",prepared,cancelled,progress,error,true));
    assert(prepared.empty() && access("ux0:data/vitagram/portrait-video.log",F_OK)!=0);
    assert(prepare_portrait_video("source.mp4",prepared,cancelled,progress,error));
    const auto output=contents(prepared), log=contents("ux0:data/vitagram/portrait-video.log");
    assert(output.size()>1024);
    const std::string first=prepared;
    // A new open discovers the saved result without permission to convert.
    assert(prepare_portrait_video("source.mp4",prepared,cancelled,progress,error,true));
    assert(prepared==first && contents(prepared)==output);
    assert(contents("ux0:data/vitagram/portrait-video.log")==log);
    assert(vault::enable({0,4,2,6,7,3,1,5,4,0,6,2})==vault::Result::Ok);
    assert(vault::seal_file("source.mp4")==vault::Convert::Done);
    assert(vault::seal_file(first)==vault::Convert::Done);
    assert(std::rename("source.mp4","moved.mp4")==0);
    assert(prepare_portrait_video("moved.mp4",prepared,cancelled,progress,error,true));
    assert(prepared==first && contents("ux0:data/vitagram/portrait-video.log")==log);
    std::cout << "8-second conversion, cache-only reopen, moved/encrypted source and output: PASS\n";
}
