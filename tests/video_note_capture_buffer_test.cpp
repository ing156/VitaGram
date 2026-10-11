#include "video_note_capture_buffer.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
using namespace video_note;
int main() {
    CaptureBuffer buffer;
    std::array<uint8_t,FrameBytes> pixels{};
    std::array<int16_t,CaptureBuffer::AudioGrain> pcm{};
    for (unsigned i=0;i<CaptureBuffer::VideoSlots;++i) {
        pixels.fill(i); assert(buffer.video(i,pixels.data())==CaptureBuffer::Result::Queued);
    }
    assert(buffer.video(999,pixels.data())==CaptureBuffer::Result::Full);
    CaptureBuffer::Item item{};
    assert(buffer.next(item) && !item.audio && item.pts==0);
    pixels.fill(200);
    assert(buffer.video(1000,pixels.data())==CaptureBuffer::Result::Full);
    assert(static_cast<const uint8_t*>(item.data)[0]==0);
    buffer.complete(item);
    assert(buffer.video(1000,pixels.data())==CaptureBuffer::Result::Queued);
    for (unsigned i=0;i<CaptureBuffer::AudioSlots;++i) {
        pcm.fill(i); assert(buffer.audio(pcm.data(),pcm.size())==CaptureBuffer::Result::Queued);
    }
    assert(buffer.audio(pcm.data(),pcm.size())==CaptureBuffer::Result::Full);
    buffer.finish();
    for (unsigned i=0;i<CaptureBuffer::AudioSlots;++i) {
        assert(buffer.next(item) && item.audio && item.bytes==pcm.size()*sizeof(int16_t));
        assert(static_cast<const int16_t*>(item.data)[0]==static_cast<int16_t>(i)); buffer.complete(item);
    }
    for (unsigned i=1;i<=CaptureBuffer::VideoSlots;++i) {
        assert(buffer.next(item) && !item.audio && item.pts==(i==CaptureBuffer::VideoSlots ? 1000 : i));
        buffer.complete(item);
    }
    assert(!buffer.next(item));
    auto stats=buffer.stats();
    assert(stats.dropped==2 && stats.video_peak==64 && stats.audio_peak==128);
    assert(buffer.video(0,pixels.data())==CaptureBuffer::Result::Stopped);
    CaptureBuffer empty;
    std::atomic<bool> started{false},done{false};
    std::thread waiting([&] { started=true; CaptureBuffer::Item next{}; assert(!empty.next(next)); done=true; });
    while (!started) std::this_thread::yield();
    empty.abort(); waiting.join(); assert(done);
    CaptureBuffer pending;
    assert(pending.audio(pcm.data(),pcm.size())==CaptureBuffer::Result::Queued);
    pending.abort(); assert(!pending.next(item));
    std::cout << "Bounded capture queues, stalled writer ownership, overflow, drain and abort: PASS\n";
}
