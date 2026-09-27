#include "audio_recorder.h"
#include "cpu_scheduler.h"
#include "config.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>
#include <fstream>
#include <functional>
#include <iterator>
static std::atomic<bool> fail_open{false}, fail_read{false};
static std::atomic<int> blocks{0}, ports{0};
namespace cpu_scheduler {
Thread::Thread(Role):id_(0) {} Thread::~Thread() {}
Media::Media(Role r):role_(r) {} Media::~Media() {}
}
int sceAudioInOpenPort(int,int,int,int) { if (fail_open) return -1; ++ports; return 1; }
int sceAudioInReleasePort(int) { --ports; return 0; }
int sceAudioInInput(int,void* out) {
    if (fail_read) return -1;
    auto* p=static_cast<int16_t*>(out);
    for (int i=0;i<512;++i) p[i]=(int16_t)(12000*std::sin((blocks*512+i)*.15));
    ++blocks; std::this_thread::sleep_for(std::chrono::milliseconds(1)); return 0;
}
static void waitFor(const std::function<bool()>& condition) {
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!condition()) { assert(std::chrono::steady_clock::now()<end); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
int main() {
    std::filesystem::create_directories(VITAGRAM_FILES_DIR);
    std::string owned;
    {
        AudioRecorder r;
        fail_open=true; assert(r.start("failed.ogg")); waitFor([&]{return !r.recording();});
        assert(r.status()=="Микрофон недоступен");
        fail_open=false; blocks=0; assert(r.start("voice.ogg"));
        waitFor([&]{return blocks>=40;}); r.stop();
        assert(!r.start("overwrite.ogg"));
        waitFor([&]{return !r.processing();});
        assert(r.conversionPercent()==100 && r.seconds()>=1 && ports==0);
        owned=r.takeCompleted(); assert(!owned.empty());
        assert(r.takeCompleted().empty());
        std::ifstream file(owned,std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),{});
        assert(bytes.size()>100 && std::string(bytes.begin(),bytes.begin()+4)=="OggS");
        assert(std::string(bytes.begin(),bytes.end()).find("OpusHead")!=std::string::npos);
        bool eos=false; size_t offset=0; uint64_t granule=0;
        while (offset<bytes.size()) {
            assert(offset+27<=bytes.size()); int segments=bytes[offset+26];
            assert(offset+27+segments<=bytes.size()); size_t body=0;
            for (int i=0;i<segments;++i) body+=bytes[offset+27+i];
            granule=0; for(int i=7;i>=0;--i) granule=(granule<<8)|bytes[offset+6+i];
            eos=(bytes[offset+5]&4)!=0; offset+=27+segments+body;
        }
        assert(offset==bytes.size() && eos && granule>=48000);
        r.cancel(); assert(std::filesystem::exists(owned)); // Upload owns the file now.
        fail_read=true; assert(r.start("bad-read.ogg")); waitFor([&]{return !r.recording();});
        r.stop(); assert(!r.processing() && r.status()=="Ошибка чтения микрофона");
        assert(r.takeCompleted().empty()); fail_read=false;
        assert(r.start("cancel.ogg")); waitFor([&]{return r.peak()>0;}); r.cancel();
        assert(ports==0 && !r.recording());
    }
    assert(std::filesystem::exists(owned));
    std::puts("Audio: real PCM to Ogg/Opus, capture errors, retry, ownership and cancellation passed");
}
