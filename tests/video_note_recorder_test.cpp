#include "test_language.h"
#include "video_note_recorder.h"
#include "video_note_encoder.h"
#include "video_note_frame.h"
#include "cpu_scheduler.h"
#include <psp2/camera.h>
#include <vita2d.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using State=VideoNoteRecorder::State;
using namespace video_note;
static unsigned char camera_memory[1024*1024];
static bool allocated=false, opened=false, streaming=false, audio=false;
static int device=-1, textures=0;
static std::atomic<bool> bad_rear{false}, bad_audio{false}, hold_encode{false};
static std::atomic<int> front_opens{0}, rear_opens{0};
struct vita2d_texture { std::vector<uint8_t> pixels=std::vector<uint8_t>(Side*Side*4); };
namespace cpu_scheduler {
Thread::Thread(Role):id_(0) {} Thread::~Thread() {}
Media::Media(Role role):role_(role) {} Media::~Media() {}
}
int sceKernelAllocMemBlock(const char*,int,size_t size,void*) { assert(!allocated && size==sizeof(camera_memory)); allocated=true; return 1; }
int sceKernelGetMemBlockBase(int,void** out) { *out=camera_memory; return 0; }
int sceKernelFreeMemBlock(int) { assert(allocated && !opened && !streaming); allocated=false; return 0; }
int sceCameraOpen(int d,SceCameraInfo* info) {
    assert(allocated && !opened && info->resolution==SCE_CAMERA_RESOLUTION_320_240);
    if (d==1 && bad_rear) return -8;
    device=d; opened=true; if (d) ++rear_opens; else ++front_opens;
    std::memset(camera_memory,128,sizeof(camera_memory)); std::memset(camera_memory,d ? 160 : 100,320*240); return 0;
}
int sceCameraStart(int d) { assert(opened && d==device); streaming=true; return 0; }
int sceCameraRead(int d,SceCameraRead* r) { assert(streaming && d==device && r->mode==1); static uint64_t number=0; r->frame=++number; return 0; }
int sceCameraStop(int d) { assert(streaming && d==device); streaming=false; return 0; }
int sceCameraClose(int d) { assert(opened && !streaming && d==device); opened=false; return 0; }
int sceAudioInOpenPort(int,int grain,int rate,int) { assert(!audio && grain==512 && rate==SampleRate); if (bad_audio) return -1; audio=true; return 1; }
int sceAudioInInput(int,void* buffer) {
    assert(audio); auto* p=static_cast<int16_t*>(buffer); for (int i=0;i<512;++i) p[i]=1000;
    std::this_thread::sleep_for(std::chrono::milliseconds(1)); return 0;
}
int sceAudioInReleasePort(int) { assert(audio); audio=false; return 0; }
int sceKernelDelayThread(unsigned) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); return 0; }
void vita2d_wait_rendering_done() {}
vita2d_texture* vita2d_create_empty_texture(unsigned w,unsigned h) { assert(w==Side && h==Side); ++textures; return new vita2d_texture; }
void* vita2d_texture_get_datap(vita2d_texture* t) { return t->pixels.data(); }
unsigned vita2d_texture_get_stride(vita2d_texture*) { return Side*4; }
void vita2d_free_texture(vita2d_texture* t) { --textures; delete t; }
bool exists(const std::string& path) { FILE* f=std::fopen(path.c_str(),"rb"); if (!f) return false; std::fclose(f); return true; }
bool encode_video_note(const std::string& raw,const std::string& pcm,const std::string& out,uint32_t samples,
    const std::atomic<bool>& cancel,std::atomic<int>& progress,std::string&) {
    assert(!opened && !streaming && !audio && !allocated);
    assert(samples>=SampleRate && samples<=SampleRate*MaxSeconds && exists(pcm));
    FILE* f=std::fopen(raw.c_str(),"rb"); assert(f);
    std::vector<uint8_t> frame(FrameBytes); uint32_t pts=0,last=0; unsigned count=0;
    while (std::fread(&pts,4,1,f)==1) {
        assert((count ? pts>last : pts==0) && pts<samples); last=pts; ++count;
        assert(std::fread(frame.data(),1,FrameBytes,f)==FrameBytes && frame[0]==16 && frame[Side*Side]==128);
    }
    assert(count); std::fclose(f);
    while (hold_encode && !cancel) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (cancel) return false;
    f=std::fopen(out.c_str(),"wb"); assert(f); std::fputs("mock MP4",f); std::fclose(f); progress=100; return true;
}
template<class F> void until(F predicate) {
    for (int i=0;i<5000 && !predicate();++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(predicate());
}
void clean(VideoNoteRecorder& r,const std::string& path,bool keep=false) {
    assert(!r.active() && !allocated && !opened && !streaming && !audio && textures==0);
    assert(!exists(path+".pcm") && !exists(path+".frames") && exists(path)==keep);
}
int main(int argc,char** argv) {
    use_test_language();
    assert(argc==2); const std::string path=std::string(argv[1])+"/capture.mp4";
    VideoNoteRecorder r;
    auto preview=[&] { assert(r.open(path)); until([&]{return r.state()==State::Preview;}); assert(!r.rearCamera()); };
    auto record=[&] { r.record(); until([&]{return r.seconds()>=2;}); assert(r.state()==State::Recording); };
    preview(); r.switchCamera(); until([&]{return r.rearCamera();}); r.cancel(); clean(r,path);
    preview(); record(); r.switchCamera(); until([&]{return r.rearCamera();});
    r.switchCamera(); until([&]{return !r.rearCamera();}); r.finish(); until([&]{return r.state()==State::Review;});
    assert(exists(path) && r.percent()==100); r.upload(); r.cancel(); clean(r,path);
    preview(); record(); r.finish(); until([&]{return r.state()==State::Review;}); r.releaseFile(); clean(r,path,true); std::remove(path.c_str());
    preview(); record(); r.cancel(); clean(r,path);
    hold_encode=true; preview(); record(); r.finish(); until([&]{return r.state()==State::Encoding;}); r.cancel(); clean(r,path); hold_encode=false;
    bad_rear=true; preview(); int prior=front_opens; r.switchCamera(); until([&]{return front_opens>prior;});
    assert(!r.rearCamera()); r.cancel(); clean(r,path); bad_rear=false;
    bad_audio=true; preview(); r.record(); until([&]{return r.state()==State::Error;}); r.cancel(); clean(r,path); bad_audio=false;
    // The audio clock stops at 60 s and automatically transitions to review.
    preview(); r.record(); until([&]{return r.state()==State::Review;}); assert(r.seconds()==60); r.cancel(); clean(r,path);
    assert(front_opens>0 && rear_opens>0);
    std::cout << "Front/rear switch, audio error, cancel at every stage, 60s limit and upload ownership: PASS\n";
}
