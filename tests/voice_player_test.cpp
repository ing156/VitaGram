#include "voice_player.h"
#include "download_avio.h"
#include "vault_avio.h"
#include "cpu_scheduler.h"
#include "test_language.h"
#include <psp2/audioout.h>
#include <cassert>
#include <chrono>
#include <thread>
#include <fstream>
#include <future>
#include <vector>
#include <sys/stat.h>

static std::atomic<int> output_volume{-1}, output_blocks{0}, ports{0};
static std::atomic<bool> output_error{false}, volume_error{false};
int sceKernelDelayThread(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); return 0; }
int64_t sceKernelGetSystemTimeWide() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int sceKernelGetThreadCurrentPriority() { return 30; }
int sceKernelGetThreadId() { return 1; }
int sceKernelChangeThreadPriority(int,int) { return 0; }
int sceAudioOutOpenPort(SceAudioOutPortType,int frames,int hz,SceAudioOutMode) {
    assert(frames==1024 && hz==48000); ++ports; return 1;
}
int sceAudioOutReleasePort(int) { --ports; return 0; }
int sceAudioOutGetRestSample(int) { return 0; }
int sceAudioOutSetVolume(int,SceAudioOutChannelFlag,int* levels) {
    assert(levels[0]==levels[1] && levels[0]>=0 && levels[0]<=SCE_AUDIO_VOLUME_0DB);
    output_volume=levels[0]; return volume_error ? -1 : 0;
}
int sceAudioOutOutput(int,const void* pcm) {
    if (!pcm) return 0;
    ++output_blocks; sceKernelDelayThread(1000);
    return output_error ? -1 : 0;
}
namespace cpu_scheduler {
Thread::Thread(Role):id_(0) {} Thread::~Thread() {}
Media::Media(Role role):role_(role) {} Media::~Media() {}
}
// File encryption is covered by the vault tests; use ordinary files at this boundary.
namespace vault {
bool is_sealed(const std::string&) { return false; }
int64_t plain_size(const std::string& path) { struct stat info{}; return stat(path.c_str(),&info) ? -1 : info.st_size; }
void Reader::close() {}
void ReadAhead::close() {}
}
int VaultAvio::attach(AVFormatContext*,const std::string&) { return 0; }
void VaultAvio::release() {}

template<class Ready> static void wait_for(Ready ready) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    while (!ready() && std::chrono::steady_clock::now()<end) sceKernelDelayThread(1000);
    assert(ready());
}
static void write_wav(const std::string& path, uint32_t padding=0) {
    std::ofstream file(path,std::ios::binary);
    auto word=[&](uint32_t value,int bytes) { while (bytes--) { file.put(value&255); value>>=8; } };
    file.write("RIFF",4); word(36+48000*10*4+(padding ? 8+padding : 0),4); file.write("WAVE",4);
    if (padding) {
        file.write("JUNK",4); word(padding,4);
        file.seekp(padding-1,std::ios::cur); file.put(0);
    }
    file.write("fmt ",4); word(16,4);
    word(1,2); word(2,2); word(48000,4); word(48000*4,4); word(4,2); word(16,2);
    file.write("data",4); word(48000*10*4,4);
    for (int i=0;i<48000*10;++i) { word((i%100)*100,2); word((i%100)*100,2); }
    assert(file.good());
}
int main(int argc,char** argv) {
    assert(argc>=2); use_test_language();
    const std::string path=std::string(argv[1])+"/audio.wav";
    write_wav(path);
    VoicePlayer player;
    player.setVolume(-10); assert(player.volume()==0);
    player.setSource(path,false,true); player.toggle();
    wait_for([&] { return player.positionSeconds()>0.2; });
    assert(output_volume==0 && player.playing());
    player.setPlaying(false); sceKernelDelayThread(10000);
    const int paused_blocks=output_blocks;
    player.seekTo(2000000); player.setSpeed(150);
    assert(!player.playing() && player.speed()==150 && player.seconds()==2);
    sceKernelDelayThread(20000); assert(output_blocks==paused_blocks);
    player.setVolume(50); player.setPlaying(true);
    wait_for([&] { return player.positionSeconds()>2.2; });
    assert(output_volume==SCE_AUDIO_VOLUME_0DB/2);
    player.setSpeed(200); player.setVolume(200);
    wait_for([&] { return player.positionSeconds()>2.5 && output_volume==SCE_AUDIO_VOLUME_0DB; });
    assert(player.speed()==200 && player.volume()==100);
    player.setPlaying(false); player.seekTo(5000000); player.setPlaying(true);
    wait_for([&] { return player.finished(); });
    assert(player.error().empty() && !player.playing());
    assert(player.positionSeconds()>9.8 && player.positionSeconds()<10.2);
    player.toggle(); wait_for([&] { return player.positionSeconds()>0.1; });
    assert(player.playing() && player.positionSeconds()<5);
    player.setSource(""); wait_for([&] { return ports==0; });

    auto stream=std::make_shared<download_stream::State>(1);
    const auto bytes=vault::plain_size(path);
    stream->update(path,bytes,0,bytes/4,false);
    player.setSource(stream->source,false,true,stream);
    assert(std::abs(player.seekLimitUs(10000000)-1500000)<100);
    stream->update(path,bytes,0,bytes,true);
    assert(player.seekLimitUs(10000000)==9700000);
    player.setSource("");

    // A stalled video input pauses only AudioOut; resuming input drains the queued sound.
    auto video_stream = std::make_shared<download_stream::State>(77);
    video_stream->update(path, vault::plain_size(path), 0, 16, false);
    AVFormatContext* video_format = avformat_alloc_context();
    DownloadAvio video_input;
    assert(video_input.attach(video_format, video_stream, &player.videoBuffering()) == 1);
    uint8_t video_bytes[32]{};
    assert(video_format->pb->read_packet(video_format->pb->opaque, video_bytes, 32) == 16);
    auto video_read = std::async(std::launch::async, [&] {
        return video_format->pb->read_packet(video_format->pb->opaque, video_bytes, 32);
    });
    wait_for([&] { return player.videoBuffering().load(); });
    const int before_buffering = output_blocks;
    player.setSource(path,true,true); player.toggle();
    sceKernelDelayThread(100000);
    assert(player.playing() && !player.clockReady() && output_blocks == before_buffering);
    video_stream->update(path, vault::plain_size(path), 0, 48, false);
    assert(video_read.wait_for(std::chrono::seconds(1)) == std::future_status::ready && video_read.get() == 32);
    assert(!player.videoBuffering());
    wait_for([&] { return output_blocks > before_buffering; });
    player.videoBuffering() = true;
    sceKernelDelayThread(10000);
    const int buffered_blocks = output_blocks;
    sceKernelDelayThread(30000);
    assert(output_blocks == buffered_blocks && player.playing());
    player.videoBuffering() = false;
    wait_for([&] { return output_blocks > buffered_blocks; });
    player.videoFrameStalled() = true;
    sceKernelDelayThread(10000);
    const int stalled_blocks = output_blocks;
    sceKernelDelayThread(30000);
    assert(output_blocks == stalled_blocks && player.playing());
    player.videoFrameStalled() = false;
    wait_for([&] { return output_blocks > stalled_blocks; });
    player.videoFrameLagging() = true;
    sceKernelDelayThread(10000);
    const int lagging_blocks = output_blocks;
    sceKernelDelayThread(30000);
    assert(output_blocks == lagging_blocks && player.playing());
    player.videoFrameLagging() = false;
    wait_for([&] { return player.finished(); });
    assert(player.error().empty() && player.seconds()==10);
    player.setSource("");
    avformat_free_context(video_format);
    video_input.release();

    // The video-audio ring stays at 1x so changing voice/music speed cannot desynchronize video.
    player.setSource(path,true,true); player.toggle();
    wait_for([&] { return player.finished(); });
    assert(player.error().empty() && player.seconds()==10);
    player.setSource("");
    const std::string large_path=std::string(argv[1])+"/large-audio.wav";
    write_wav(large_path,170*1024*1024);
    player.setSource(large_path,true,true); player.toggle();
    wait_for([&] { return player.finished(); });
    assert(player.error().empty() && player.seconds()==10);
    player.setSource("");
    volume_error=true;
    player.setSource(path,false,true); player.toggle();
    wait_for([&] { return player.finished(); });
    assert(!player.error().empty()); volume_error=false;
    player.setSource(""); output_error=true;
    player.setSource(path,false,true); player.toggle();
    wait_for([&] { return player.finished(); });
    assert(!player.error().empty()); output_error=false;
    player.setSource("/missing-audio.wav",false,true); player.toggle();
    wait_for([&] { return player.finished(); });
    assert(!player.error().empty());
    for (int i=2;i<argc;++i) {
        double baseline=0;
        for (int speed : {100,200}) {
            player.setSource(""); player.setSpeed(speed);
            player.setSource(argv[i],false,true); player.toggle();
            wait_for([&] { return player.finished(); });
            assert(player.error().empty());
            if (speed==100) baseline=player.positionSeconds();
            else assert(std::abs(player.positionSeconds()-baseline)<0.2);
        }
        if (baseline>31) {
            player.setSource(""); player.setSource(argv[i],false,true); player.seekTo(30000000); player.toggle();
            wait_for([&] { return player.finished(); });
            assert(player.error().empty() && std::abs(player.positionSeconds()-baseline)<0.3);
        }
    }
    player.shutdown(); assert(ports==0);
    std::puts("VoicePlayer: PCM, media clock, pause, seek, speed, replay, volume, streaming limits, video ring and output failures passed");
}
