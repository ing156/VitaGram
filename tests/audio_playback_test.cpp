#include "audio_bar.h"
#include "audio_tempo.h"
#include <cassert>
#include <cmath>
#include <vector>
#include <cstdio>

int main() {
    using namespace audio_bar;
    for (float left : {0.f,224.f}) for (float bottom : {494.f,544.f}) {
        Layout layout{left,bottom};
        assert(layout.titleWidth() >= 260);
        assert(layout.titleX()+layout.titleWidth() < 626);
        for (int i=0; i<Count; ++i) {
            const auto rect = layout.cell(Control(i));
            assert(rect.x >= left && rect.x+rect.w <= 960 && rect.y >= layout.top() && rect.y+rect.h <= bottom);
            assert(layout.hit(rect.x+rect.w/2,rect.y+rect.h/2,false) == i);
        }
        for (int y=int(layout.top())-1; y<=bottom; ++y) for (int x=int(left)-1; x<=960; ++x) {
            int covered=0;
            for (int i=0; i<Count; ++i) if (layout.cell(Control(i)).contains(x,y)) ++covered;
            assert(covered<=1);
            assert((layout.hit(x,y,false) != None) == (covered==1));
        }
        assert(layout.hit(left,layout.top()+40,false)==None);
        assert(layout.hit(left+150,layout.top()+40,false)==None);
        assert(layout.hit(960,bottom,false)==None);
        assert(layout.hit(800,layout.top()-40,true)==VolumeTrack);
        assert(layout.hit(748,layout.top()-36,true)==Mute);
        assert(layout.hit(800,layout.top()-40,false)==None);
        assert(layout.volumeAt(-100)==0 && layout.volumeAt(2000)==100);
        assert(layout.volumeAt(layout.volumeTrack().x+layout.volumeTrack().w/2)==50);
        assert(layout.seekAt(-1,10000000,4000000)==0);
        assert(layout.seekAt(2000,10000000,4000000)==4000000);
        assert(layout.seekAt(500,0,0)==0);
    }
    int speed=100;
    for (int i=0;i<7;++i) speed=nextSpeed(speed);
    assert(speed==100 && nextSpeed(100,-1)==75 && nextSpeed(200)==50);
    Shoulders shoulders;
    unsigned pressed=1|4;
    assert(!shoulders.update(1,pressed,3) && pressed==4);
    pressed=2;
    assert(shoulders.update(3,pressed,3) && pressed==0);
    pressed=0;
    assert(!shoulders.update(3,pressed,3) && pressed==0);
    assert(!shoulders.update(1,pressed,3) && pressed==0);
    assert(!shoulders.update(0,pressed,3) && pressed==0);
    pressed=1;
    assert(!shoulders.update(1,pressed,3) && pressed==0);
    assert(!shoulders.update(0,pressed,3) && pressed==1);
    pressed=3;
    assert(shoulders.update(3,pressed,3) && pressed==0);

    constexpr int frames=48000*3;
    std::vector<int16_t> input(frames*2);
    for (int i=0;i<frames;++i) input[2*i]=input[2*i+1]=int16_t(12000*std::sin(i*2*3.141592653589793*440/48000));
    for (int rate : {50,75,100,125,150,175,200}) {
        AudioTempo tempo;
        assert(tempo.open(rate)>=0);
        std::vector<int16_t> pcm;
        auto collect=[&](const int16_t* data,int count) { pcm.insert(pcm.end(),data,data+count*2); return true; };
        for (int i=0;i<frames;i+=1024) assert(tempo.push(input.data()+i*2,std::min(1024,frames-i),collect)>=0);
        assert(tempo.push(nullptr,0,collect)>=0);
        const int count=pcm.size()/2;
        assert(std::abs(count-frames*100/rate)<2400);
        int crossings=0;
        for (int i=4801;i<count-4800;++i) if (pcm[2*(i-1)]<=0 && pcm[2*i]>0) ++crossings;
        const double frequency=crossings*48000.0/(count-9600);
        assert(std::abs(frequency-440)<3);
        for (int i=0;i<count;++i) assert(pcm[2*i]==pcm[2*i+1]);
        std::printf("tempo %d%%: %d frames, %.1f Hz\n",rate,count,frequency);
    }
    AudioTempo cancelled;
    assert(cancelled.open(150)>=0);
    int rc=0;
    for (int i=0;i<frames && rc>=0;i+=1024)
        rc=cancelled.push(input.data()+i*2,std::min(1024,frames-i),[](const int16_t*,int) { return false; });
    if (rc>=0) rc=cancelled.push(nullptr,0,[](const int16_t*,int) { return false; });
    assert(rc==AVERROR_EXIT);
    std::puts("Audio controls: geometry, touch boundaries, speed duration, pitch and cancellation passed");
}
