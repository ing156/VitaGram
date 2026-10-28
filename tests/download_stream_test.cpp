#include "download_avio.h"
#include "media_load_view.h"
#include "media_seek.h"
#include "media_limits.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <thread>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
}
using namespace std::chrono_literals;

#ifdef VITAGRAM_TEST_SCEIO
static std::atomic<int> native_reads{0};
static std::atomic<bool> native_failure{false};
static std::atomic<bool> native_interrupt{false};
int sceIoOpen(const char* path, int, int) { return ::open(path, O_RDONLY); }
int sceIoClose(int fd) { return ::close(fd); }
int sceIoPread(int fd, void* buffer, unsigned size, int64_t offset) {
    ++native_reads;
    if (native_interrupt.exchange(false)) return static_cast<int>(0x80010004u);
    if (native_failure) return -1;
    return static_cast<int>(::pread(fd, buffer, size, offset));
}
#endif

static int cancelled(void* opaque) { return static_cast<std::atomic<bool>*>(opaque)->load(); }

static void raw_reads() {
    const std::string path = "gaps.bin";
    std::vector<uint8_t> bytes(1024 * 1024, 0xcc);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    auto state = std::make_shared<download_stream::State>(1);
    state->update(path, bytes.size(), 64, bytes.size() - 64, false);
    assert(!state->ready()); // A prefix from another offset does not cover the start.
    state->update(path, bytes.size(), 0, 16, false);
    AVFormatContext* format = avformat_alloc_context();
    std::atomic<bool> stop{false};
    std::atomic<bool> video_waiting{false};
    format->interrupt_callback = {cancelled, &stop};
    DownloadAvio io;
    assert(io.attach(format, state, &video_waiting) == 1);
    assert(download_stream::reading());
    uint8_t buffer[64]{};
    // Use callbacks directly so AVIO buffering cannot conceal a read of a gap.
    assert(format->pb->read_packet(format->pb->opaque, buffer, 64) == 16);
#ifdef VITAGRAM_TEST_SCEIO
    assert(native_reads > 0);
    native_failure = true;
    assert(format->pb->seek(format->pb->opaque, 0, SEEK_SET) == 0);
    assert(format->pb->read_packet(format->pb->opaque, buffer, 64) == AVERROR(EIO));
    native_failure = false;
    native_interrupt = true;
    assert(format->pb->read_packet(format->pb->opaque, buffer, 64) == 16);
    assert(!native_interrupt);
    assert(format->pb->seek(format->pb->opaque, 16, SEEK_SET) == 16);
#endif
    auto blocked = std::async(std::launch::async, [&] { return format->pb->read_packet(format->pb->opaque, buffer, 64); });
    assert(blocked.wait_for(50ms) == std::future_status::timeout);
    assert(!video_waiting);
    assert(blocked.wait_for(600ms) == std::future_status::timeout && video_waiting);
    if (performance::enabled) {
        FILE* trace = std::tmpfile(); assert(trace);
        stream_diagnostics::write(trace); std::rewind(trace);
        char line[512]{}; assert(std::fgets(line, sizeof(line), trace)); std::fclose(trace);
        assert(std::strstr(line, "role=video stage=wait_prefix"));
        assert(std::strstr(line, "position=16 prefix=16"));
        assert(std::strstr(line, "tail_begin=0 tail_end=0 tail_active=0"));
    }
    state->update(path, bytes.size(), 0, 80, false);
    assert(blocked.wait_for(500ms) == std::future_status::ready && blocked.get() == 64);
    assert(!video_waiting);
    blocked = std::async(std::launch::async, [&] { return format->pb->read_packet(format->pb->opaque, buffer, 64); });
    assert(blocked.wait_for(600ms) == std::future_status::timeout && video_waiting);
    stop = true;
    assert(blocked.wait_for(500ms) == std::future_status::ready);
    assert(blocked.get() == AVERROR_EXIT);
    assert(!video_waiting);
    stop = false;
    assert(format->pb->seek(format->pb->opaque, 0, AVSEEK_SIZE) == int64_t(bytes.size()));
    assert(format->pb->seek(format->pb->opaque, -1, SEEK_SET) < 0);
    assert(format->pb->seek(format->pb->opaque, INT64_MAX, SEEK_CUR) < 0);
    assert(format->pb->seek(format->pb->opaque, -8, SEEK_END) == int64_t(bytes.size()) - 8);
    state->update(path, bytes.size(), 0, bytes.size(), true);
    assert(format->pb->read_packet(format->pb->opaque, buffer, 64) == 8);
    assert(format->pb->read_packet(format->pb->opaque, buffer, 64) == AVERROR_EOF);
    avformat_free_context(format);
    io.release();
    assert(!download_stream::reading());
}

static void video_buffer_window() {
    const std::string path = "video-reserve.bin";
    const int64_t size = 12 * 1024 * 1024;
    { std::ofstream file(path, std::ios::binary); file.seekp(size - 1); file.put('x'); }
    auto state = std::make_shared<download_stream::State>(43);
    state->update(path, size, 0, 256 * 1024, false);
    assert(state->ready()); // UI can create the player and retain the play request.
    AVFormatContext* format = avformat_alloc_context(); assert(format);
    std::atomic<bool> cancelled_flag{false}, waiting{false};
    format->interrupt_callback = {cancelled, &cancelled_flag};
    DownloadAvio io;
    auto opening = std::async(std::launch::async, [&] {
        return io.attach(format, state, &waiting, download_stream::video_resume_bytes, download_stream::video_start_bytes);
    });
    assert(opening.wait_for(100ms) == std::future_status::timeout && waiting);
    if (performance::enabled) {
        bool observed = false;
        for (int attempt = 0; attempt < 50 && !observed; ++attempt) {
            FILE* trace = std::tmpfile(); assert(trace);
            stream_diagnostics::write(trace); std::rewind(trace);
            char line[512]{}; const bool present = std::fgets(line, sizeof(line), trace) != nullptr; std::fclose(trace);
            observed = present && std::strstr(line, "role=video stage=startup_buffer") &&
                std::strstr(line, "position=0 prefix=262144");
            if (!observed) std::this_thread::sleep_for(10ms);
        }
        assert(observed);
    }
    state->update(path, size, 0, 4 * 1024 * 1024, false);
    assert(opening.wait_for(100ms) == std::future_status::timeout && waiting);
    state->update(path, size, 0, download_stream::video_start_bytes, false);
    assert(opening.wait_for(500ms) == std::future_status::ready && opening.get() == 1 && !waiting);
    assert(format->pb->buffer_size == 256 * 1024);
    state->update(path, size, 0, 16, false);
    uint8_t buffer[64]{};
    assert(format->pb->read_packet(format->pb->opaque, buffer, sizeof(buffer)) == 16);
    auto read = std::async(std::launch::async, [&] { return format->pb->read_packet(format->pb->opaque, buffer, sizeof(buffer)); });
    assert(read.wait_for(600ms) == std::future_status::timeout && waiting);
    state->update(path, size, 0, 16 + 128 * 1024, false);
    assert(read.wait_for(100ms) == std::future_status::timeout && waiting);
    state->update(path, size, 0, 16 + download_stream::video_resume_bytes, false);
    assert(read.wait_for(500ms) == std::future_status::ready && read.get() == 64 && !waiting);
    const int64_t near_end = size - 1024;
    assert(format->pb->seek(format->pb->opaque, near_end, SEEK_SET) == near_end);
    state->update(path, size, 0, near_end, false);
    read = std::async(std::launch::async, [&] { return format->pb->read_packet(format->pb->opaque, buffer, sizeof(buffer)); });
    assert(read.wait_for(100ms) == std::future_status::timeout);
    state->update(path, size, 0, near_end + 512, false);
    assert(read.wait_for(100ms) == std::future_status::timeout);
    state->update(path, size, 0, size, true);
    assert(read.wait_for(500ms) == std::future_status::ready && read.get() == 64);
    avformat_free_context(format); io.release();
    auto short_file = std::make_shared<download_stream::State>(44);
    short_file->update(path, 100, 0, 100, true);
    format = avformat_alloc_context(); assert(format);
    assert(io.attach(format, short_file, &waiting, download_stream::video_resume_bytes, download_stream::video_start_bytes) == 1);
    avformat_free_context(format); io.release();
    state->update(path, size, 0, 16, false);
    format = avformat_alloc_context(); assert(format);
    format->interrupt_callback = {cancelled, &cancelled_flag};
    opening = std::async(std::launch::async, [&] {
        return io.attach(format, state, &waiting, download_stream::video_resume_bytes, download_stream::video_start_bytes);
    });
    assert(opening.wait_for(100ms) == std::future_status::timeout && waiting);
    cancelled_flag = true;
    assert(opening.wait_for(500ms) == std::future_status::ready && opening.get() == AVERROR_EXIT && !waiting);
    avformat_free_context(format);
    std::puts("Video window: 8 MiB startup, 4 MiB resume and bounded end-of-file reserve passed");
}

static void completed_snapshot() {
    const char* path = "completed.bin";
    { std::ofstream file(path, std::ios::binary); file << "abcdefghijklmnop"; }
    auto state = std::make_shared<download_stream::State>(42);
    state->update(path, 16, 0, 16, true);
    AVFormatContext* format = avformat_alloc_context(); assert(format);
    DownloadAvio io; assert(io.attach(format, state, nullptr, 0, 0, true) == 1);
    state->update("", 0, 0, 0, false); // A stale progress snapshot must not invalidate an open full file.
    assert(format->pb->seek(format->pb->opaque, 8, SEEK_SET) == 8);
    assert(format->pb->seek(format->pb->opaque, 0, AVSEEK_SIZE) == 16);
    uint8_t data[8]{};
    assert(format->pb->read_packet(format->pb->opaque, data, 8) == 8);
    assert(std::memcmp(data, "ijklmnop", 8) == 0);
    assert(format->pb->seek(format->pb->opaque, 0, SEEK_SET) == 0);
    assert(::truncate(path, 0) == 0);
    assert(format->pb->read_packet(format->pb->opaque, data, 8) == AVERROR(EIO));
    avformat_free_context(format); io.release();
    std::puts("Completed file: stale progress ignored; truncated file remains an error");
}

static void large_reads() {
    const int64_t size=media_limits::video_bytes;
    FILE* file=std::fopen("large.bin","wb"); assert(file);
    assert(std::fseek(file,size-1,SEEK_SET)==0 && std::fputc(0,file)!=EOF);
    const int64_t offsets[]={128LL*1024*1024-8, 256LL*1024*1024, size-32};
    const uint8_t marker[32]={0x12,0x34,0x56,0x78};
    for (const auto at:offsets) {
        assert(std::fseek(file,at,SEEK_SET)==0);
        assert(std::fwrite(marker,1,sizeof(marker),file)==sizeof(marker));
    }
    assert(std::fclose(file)==0);
    auto state=std::make_shared<download_stream::State>(99);
    state->update("large.bin",size,0,129LL*1024*1024,false);
    auto* format=avformat_alloc_context(); assert(format);
    DownloadAvio io; assert(io.attach(format,state)==1);
    uint8_t buffer[32]{};
    auto* pb=format->pb;
    assert(pb->seek(pb->opaque,0,AVSEEK_SIZE)==size);
    assert(pb->seek(pb->opaque,offsets[0],SEEK_SET)==offsets[0]);
    assert(pb->read_packet(pb->opaque,buffer,sizeof(buffer))==32 && !std::memcmp(buffer,marker,32));
    assert(pb->seek(pb->opaque,offsets[1],SEEK_SET)==offsets[1]);
    auto waiting=std::async(std::launch::async,[&] { return pb->read_packet(pb->opaque,buffer,sizeof(buffer)); });
    assert(waiting.wait_for(50ms)==std::future_status::timeout);
    state->update("large.bin",size,0,size,true);
    assert(waiting.wait_for(500ms)==std::future_status::ready && waiting.get()==32);
    assert(!std::memcmp(buffer,marker,32));
    assert(pb->seek(pb->opaque,-32,SEEK_END)==size-32);
    assert(pb->read_packet(pb->opaque,buffer,sizeof(buffer))==32 && !std::memcmp(buffer,marker,32));
    assert(pb->read_packet(pb->opaque,buffer,sizeof(buffer))==AVERROR_EOF);
    avformat_free_context(format); io.release();
    std::puts("1 GiB sparse stream: 128 MiB boundary, growing prefix, tail seek and EOF passed");
}

// Remux the fixture with interleaved audio/video and either front or trailing metadata.
static void faststart(const char* source, bool front = true) {
    const char* target = front ? "fast.mp4" : "tail.mp4";
    AVFormatContext *in = nullptr, *out = nullptr;
    assert(avformat_open_input(&in, source, nullptr, nullptr) >= 0);
    assert(avformat_alloc_output_context2(&out, nullptr, "mp4", target) >= 0);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        auto* stream = avformat_new_stream(out, nullptr);
        assert(stream && avcodec_parameters_copy(stream->codecpar, in->streams[i]->codecpar) >= 0);
        stream->codecpar->codec_tag = 0;
        stream->time_base = in->streams[i]->time_base;
    }
    assert(avio_open(&out->pb, target, AVIO_FLAG_WRITE) >= 0);
    AVDictionary* options = nullptr;
    if (front) av_dict_set(&options, "movflags", "faststart", 0);
    assert(avformat_write_header(out, &options) >= 0);
    av_dict_free(&options);
    auto* packet = av_packet_alloc();
    int rc;
    while ((rc = av_read_frame(in, packet)) >= 0) {
        av_packet_rescale_ts(packet, in->streams[packet->stream_index]->time_base, out->streams[packet->stream_index]->time_base);
        packet->pos = -1;
        assert(av_interleaved_write_frame(out, packet) >= 0);
    }
    assert(rc == AVERROR_EOF && av_write_trailer(out) >= 0);
    av_packet_free(&packet);
    avio_closep(&out->pb); avformat_free_context(out); avformat_close_input(&in);
    FILE* file = std::fopen(target, "ab"); assert(file);
    const uint8_t trailing[] = {0, 0, 0, 16, 'f', 'r', 'e', 'e', 0, 0, 0, 0, 0, 0, 0, 0};
    assert(std::fwrite(trailing, 1, sizeof(trailing), file) == sizeof(trailing));
    assert(std::fclose(file) == 0);
}

static int decode(download_stream::Shared state, AVMediaType type, std::atomic<bool>& first, std::atomic<bool>* opened = nullptr, bool discard_others = true) {
    auto* format = avformat_alloc_context();
    DownloadAvio io;
    assert(io.attach(format, state, nullptr, 0, 0, true) == 1);
    format->probesize = 256 * 1024;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist", "mov,mp4,m4a,3gp,3g2,mj2", 0);
    assert(avformat_open_input(&format, state->source.c_str(), nullptr, &options) >= 0);
    io.finishOpen(format);
    av_dict_free(&options);
    int index = av_find_best_stream(format, type, -1, -1, nullptr, 0);
    assert(index >= 0);
    if (discard_others) for (unsigned i = 0; i < format->nb_streams; ++i)
        if (static_cast<int>(i) != index) format->streams[i]->discard = AVDISCARD_ALL;
    const auto* params = format->streams[index]->codecpar;
    auto* codec = avcodec_alloc_context3(avcodec_find_decoder(params->codec_id));
    assert(codec && avcodec_parameters_to_context(codec, params) >= 0);
    assert(avcodec_open2(codec, codec->codec, nullptr) >= 0);
    if (opened) *opened = true; // Same milestone the player publishes before its paused decode loop.
    auto* packet = av_packet_alloc();
    auto* frame = av_frame_alloc();
    int frames = 0;
    auto drain = [&] {
        int rc;
        while ((rc = avcodec_receive_frame(codec, frame)) >= 0) {
            ++frames; first = true; av_frame_unref(frame);
        }
        assert(rc == AVERROR(EAGAIN) || rc == AVERROR_EOF);
    };
    int rc;
    while ((rc = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == index) {
            assert(avcodec_send_packet(codec, packet) >= 0);
            drain();
        }
        av_packet_unref(packet);
    }
    assert(rc == AVERROR_EOF);
    assert(avcodec_send_packet(codec, nullptr) >= 0);
    drain();
    av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&codec);
    avformat_close_input(&format);
    io.release();
    return frames;
}

static void large_mp4() {
    std::ifstream in("fast.mp4",std::ios::binary);
    std::ofstream out("large.mp4",std::ios::binary);
    out << in.rdbuf(); out.close();
    FILE* file=std::fopen("large.mp4","r+b"); assert(file);
    assert(std::fseek(file,0,SEEK_END)==0);
    const int64_t size=170LL*1024*1024;
    const uint32_t free_size=uint32_t(size-std::ftell(file));
    const uint8_t atom[]={uint8_t(free_size>>24),uint8_t(free_size>>16),uint8_t(free_size>>8),uint8_t(free_size),'f','r','e','e'};
    assert(free_size>8 && std::fwrite(atom,1,sizeof(atom),file)==sizeof(atom));
    assert(std::fseek(file,size-1,SEEK_SET)==0 && std::fputc(0,file)!=EOF && std::fclose(file)==0);
    auto state=std::make_shared<download_stream::State>(100);
    state->update("large.mp4",size,0,size,true);
    std::atomic<bool> video{false},audio{false};
    assert(decode(state,AVMEDIA_TYPE_VIDEO,video)>0);
    assert(decode(state,AVMEDIA_TYPE_AUDIO,audio)>0);
    assert(video && audio);
    std::puts("170 MiB sparse MP4: real H.264/AAC decoding through bounded input passed");
}

static void growing_mp4() {
    std::ifstream input("fast.mp4", std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    assert(bytes.size() > 2 * 1024 * 1024);
    const size_t prefix = 1024 * 1024;
    std::ofstream output("partial.mp4", std::ios::binary);
    output.write(bytes.data(), prefix); output.flush();
    auto state = std::make_shared<download_stream::State>(2);
    state->update("partial.mp4", bytes.size(), 0, prefix, false);
    assert(state->ready());
    std::atomic<bool> video{false}, audio{false}, opened{false};
    auto video_job = std::async(std::launch::async, [&] { return decode(state, AVMEDIA_TYPE_VIDEO, video, &opened); });
    auto audio_job = std::async(std::launch::async, [&] { return decode(state, AVMEDIA_TYPE_AUDIO, audio); });
    auto deadline = std::chrono::steady_clock::now() + 10s;
    while ((!video || !audio) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
    assert(video && audio && !state->snapshot().complete); // Both decode before full download.
    const auto streaming_view = MediaLoadView::make(bytes.size(), prefix, false, opened);
    assert(streaming_view.controls && streaming_view.downloading && !streaming_view.waiting);
    assert(streaming_view.percent > 0 && streaming_view.percent < 100); // Controls must not hide the remaining download.
    assert(video_job.wait_for(50ms) == std::future_status::timeout);
    assert(audio_job.wait_for(50ms) == std::future_status::timeout);
    for (size_t at = prefix; at < bytes.size();) {
        size_t count = std::min<size_t>(64 * 1024, bytes.size() - at);
        output.write(bytes.data() + at, count); output.flush(); at += count;
        state->update("partial.mp4", bytes.size(), 0, at, false);
        std::this_thread::sleep_for(1ms);
    }
    output.close();
    assert(std::rename("partial.mp4", "complete.mp4") == 0);
    state->update("complete.mp4", bytes.size(), 0, bytes.size(), true);
    assert(video_job.wait_for(20s) == std::future_status::ready);
    assert(audio_job.wait_for(20s) == std::future_status::ready);
    int video_frames = video_job.get(), audio_frames = audio_job.get();
    auto complete = std::make_shared<download_stream::State>(3);
    complete->update("fast.mp4", bytes.size(), 0, bytes.size(), true);
    assert(video_frames > 0 && video_frames == decode(complete, AVMEDIA_TYPE_VIDEO, video, nullptr, false));
    assert(audio_frames > 0 && audio_frames == decode(complete, AVMEDIA_TYPE_AUDIO, audio, nullptr, false));
    assert(!download_stream::reading());
    std::printf("Progressive MP4: %d video frames, %d audio frames; early start, starvation, rename and complete-file parity passed\n", video_frames, audio_frames);
}

static void tail_windows() {
    using download_stream::State;
    constexpr int64_t size = 32 * 1024 * 1024, head = 8 * 1024 * 1024, tail = size - 1024 * 1024;
    State state(10);
    state.update("partial", size, 0, head, false);
    state.requestTail(tail); assert(state.pollTail(1, true) == -1);
    state.beginOpen(); state.beginOpen();
    state.requestTail(head + 2 * 1024 * 1024); assert(state.pollTail(1, true) == -1);
    state.requestTail(tail);
    assert(state.pollTail(1, true) == tail);
    state.requestTail(tail + 65536); assert(state.pollTail(2, true) == -1);
    state.endOpen(); // The second reader still needs the same header.
    state.update("partial", size, tail, 65536, false);
    auto snapshot = state.snapshot();
    assert(snapshot.prefix == head && snapshot.available(head) == 0);
    assert(snapshot.available(tail) == 65536 && snapshot.available(tail + 65536) == 0);
    assert(state.pollTail(3, true) == -1);
    state.update("partial", size, tail, size - tail, false);
    assert(state.pollTail(4, true) == 0 && !state.tailActive());
    state.update("partial", size, 0, head + 65536, false);
    assert(state.snapshot().available(tail) == size - tail);
    state.endOpen(); state.requestTail(tail); assert(state.pollTail(5, true) == -1);
    state.update("replacement", size, 0, 16, false);
    assert(state.snapshot().available(tail) == 0 && state.snapshot().prefix == 16);
    for (int reason = 0; reason < 4; ++reason) {
        State fallback(11);
        fallback.update("partial", size, 0, head, false);
        fallback.beginOpen(); fallback.requestTail(tail);
        assert(fallback.pollTail(1, true) == tail);
        if (reason == 0) fallback.tailError(tail);
        if (reason == 1) fallback.endOpen();
        const auto action = fallback.pollTail(reason == 2 ? download_stream::tail_timeout_us + 1 : 2, reason != 3);
        assert(action == (reason == 3 ? -2 : 0));
        assert(fallback.pollTail(download_stream::tail_timeout_us + 2, true) == -1);
    }
    State slow(13);
    slow.update("partial", size, 0, head, false);
    slow.beginOpen(); slow.requestTail(tail); assert(slow.pollTail(1, true) == tail);
    assert(slow.pollTail(58478809, true) == -1 && slow.tailActive()); // Real Vita's 58.48 s file growth.
    slow.update("partial", size, tail, size - tail, false);
    assert(slow.snapshot().available(tail) == size - tail && slow.pollTail(58478810, true) == 0);
    slow.endOpen();
    State reopened(14);
    reopened.update("partial", size, 0, head, false);
    reopened.beginOpen(); reopened.requestTail(tail); assert(reopened.pollTail(1, true) == tail);
    const auto previous_generation = reopened.generation;
    reopened.endOpen(); assert(reopened.pollTail(2, true) == 0);
    // Late updates remain valid after the ordinary download has been restored.
    reopened.update("partial", size, tail, 65536, false);
    assert(reopened.snapshot().available(tail) == 65536 && reopened.snapshot().prefix == head);
    reopened.beginOpen(); reopened.requestTail(tail + 65536);
    assert(reopened.pollTail(3, true) == tail + 65536 && reopened.generation != previous_generation);
    reopened.update("partial", size, tail + 65536, size - tail - 65536, false);
    assert(reopened.snapshot().available(tail) == size - tail && reopened.pollTail(4, true) == 0);
    reopened.endOpen();
    State complete(12);
    complete.update("partial", size, 0, head, false);
    complete.beginOpen(); complete.requestTail(tail); assert(complete.pollTail(1, true) == tail);
    complete.update("final", size, 0, size, true);
    assert(complete.pollTail(2, true) == -1 && complete.snapshot().available(head) == size - head);
    std::puts("MP4 ranges: shared readers, gaps, bounded tail, restore, timeout, cancellation and invalidation passed");
}

static void tail_metadata(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = int64_t(input.tellg());
    assert(size > 2 * 1024 * 1024);
    auto state = std::make_shared<download_stream::State>(4);
    state->update(path, size, 0, 1024 * 1024, false);
    std::atomic<bool> first{false}, opened{false};
    auto job = std::async(std::launch::async, [&] { return decode(state, AVMEDIA_TYPE_VIDEO, first, &opened); });
    assert(job.wait_for(200ms) == std::future_status::timeout && !first);
    assert(state->ready()); // The old UI treated this readable prefix as a ready player.
    const auto waiting_view = MediaLoadView::make(size, 1024 * 1024, false, opened);
    assert(waiting_view.downloading && waiting_view.waiting && !waiting_view.controls);
    assert(waiting_view.percent > 0 && waiting_view.percent < 100);
    std::atomic<bool> audio_first{false}, audio_opened{false};
    auto audio = std::async(std::launch::async, [&] { return decode(state, AVMEDIA_TYPE_AUDIO, audio_first, &audio_opened); });
    int64_t offset = -1;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (offset < 0 && std::chrono::steady_clock::now() < deadline) {
        offset = state->pollTail(1, true);
        std::this_thread::sleep_for(1ms);
    }
    assert(offset > 1024 * 1024 && size - offset <= download_stream::mp4_tail_bytes);
    state->update(path, size, offset, 0, false);
    assert(state->snapshot().prefix == 1024 * 1024 && state->snapshot().available(offset) == 0);
    state->update(path, size, offset, size - offset, false);
    while ((!first || !audio_first) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    assert(opened && audio_opened && first && audio_first && !state->snapshot().complete);
    assert(state->pollTail(2, true) == 0); // Restore sequential download after the tail.
    assert(state->snapshot().available(1024 * 1024) == 0); // The gap was never published.
    state->update(path, size, 0, size, true);
    const auto completed = job.wait_for(20s);
    if (completed != std::future_status::ready) stream_diagnostics::write(stderr);
    assert(completed == std::future_status::ready && job.get() > 0);
    assert(audio.wait_for(20s) == std::future_status::ready && audio.get() > 0);
    assert(first && !download_stream::reading());
    const auto completed_view = MediaLoadView::make(size, size, true, opened);
    assert(completed_view.controls && !completed_view.downloading && !completed_view.waiting);
    std::puts("MP4 tail metadata: video and audio start from head + tail before full download");
}

static void seek_target(const char* path) {
    media_seek::TimeSelection time;
    time.begin(65000000, 130000000, 500000);
    time.select(1); time.adjust(-1);
    assert(time.targetUs() == 64000000); // Seconds carry across the minute boundary.
    time.field = 1; time.adjust(1);
    assert(time.targetUs() == 124000000);
    time.field = 0; time.adjust(1);
    assert(time.targetUs() == 129000000); // Keep the target safely before EOF.
    time.adjust(-1); assert(time.targetUs() == 0);
    time.begin(1000000, 2000000, 500000); assert(time.field == 2 && time.maximum == 1);
    assert(media_seek::before_target(900000, 100000, 1000000));
    assert(!media_seek::before_target(950000, 100000, 1000000));

    // Decode the real fixture after a demuxer seek and select only the picture
    // covering the requested instant, rather than showing the preceding GOP.
    AVFormatContext* format = nullptr;
    assert(avformat_open_input(&format, path, nullptr, nullptr) >= 0);
    int index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    assert(index >= 0);
    auto* stream = format->streams[index];
    auto* codec = avcodec_alloc_context3(avcodec_find_decoder(stream->codecpar->codec_id));
    assert(codec && avcodec_parameters_to_context(codec, stream->codecpar) >= 0);
    codec->pkt_timebase = stream->time_base;
    assert(avcodec_open2(codec, codec->codec, nullptr) >= 0);
    const int64_t target = 13250000;
    const int64_t origin = format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
    assert(av_seek_frame(format, index, av_rescale_q(target + origin, AV_TIME_BASE_Q, stream->time_base), AVSEEK_FLAG_BACKWARD) >= 0);
    auto* packet = av_packet_alloc(); auto* frame = av_frame_alloc();
    int64_t displayed = -1, duration = 0;
    int skipped = 0;
    const auto rate = av_guess_frame_rate(format, stream, nullptr);
    while (displayed < 0 && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == index) {
            assert(avcodec_send_packet(codec, packet) >= 0);
            int rc;
            while ((rc = avcodec_receive_frame(codec, frame)) >= 0) {
                assert(frame->best_effort_timestamp != AV_NOPTS_VALUE);
                const int64_t at = av_rescale_q(frame->best_effort_timestamp, stream->time_base, AV_TIME_BASE_Q) - origin;
                duration = frame->duration > 0 ? av_rescale_q(frame->duration, stream->time_base, AV_TIME_BASE_Q) :
                    av_rescale_q(1, av_inv_q(rate), AV_TIME_BASE_Q);
                if (media_seek::before_target(at, duration, target)) ++skipped;
                else { displayed = at; break; }
                av_frame_unref(frame);
            }
            assert(displayed >= 0 || rc == AVERROR(EAGAIN) || rc == AVERROR_EOF);
        }
        av_packet_unref(packet);
    }
    assert(skipped > 0 && displayed >= 0 && displayed <= target && displayed + duration > target);
    av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&codec); avformat_close_input(&format);
    std::puts("Precise media seek: field carry, start/end bounds and real H.264 keyframe pre-roll passed");
}

static void read_ahead_stall() {
    struct Disk {
        std::atomic<int> calls{0};
        std::atomic<bool> hold{true};
        static int read(void* opaque, uint8_t* data, int count, int64_t offset) {
            auto& disk = *static_cast<Disk*>(opaque);
            ++disk.calls;
            if (offset == 3 * 256 * 1024)
                while (disk.hold) std::this_thread::sleep_for(1ms);
            std::memset(data, offset / (256 * 1024), count);
            return count;
        }
    } disk;
    auto state = std::make_shared<download_stream::State>(99);
    state->update("unused", 8 * 1024 * 1024, 0, 8 * 1024 * 1024, true);
    DownloadReadAhead ahead;
    assert(ahead.open(state, Disk::read, &disk));
    std::vector<uint8_t> bytes(256 * 1024);
    assert(ahead.read(bytes.data(), bytes.size(), 0, {}) == int(bytes.size()));
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (disk.calls < 4 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    assert(disk.calls == 4); // Worker is stuck on disk, with two ready blocks in RAM.
    auto cached = std::async(std::launch::async, [&] { return ahead.read(bytes.data(), bytes.size(), 256 * 1024, AVIOInterruptCB{}); });
    assert(cached.wait_for(200ms) == std::future_status::ready);
    assert(cached.get() == int(bytes.size()) && bytes.front() == 1 && bytes.back() == 1);
    std::atomic<bool> stop{true};
    assert(ahead.read(bytes.data(), bytes.size(), 6 * 256 * 1024, {cancelled, &stop}) == AVERROR_EXIT);
    disk.hold = false;
    stop = false;
    assert(ahead.read(bytes.data(), bytes.size(), 6 * 256 * 1024, {cancelled, &stop}) == int(bytes.size()));
    assert(bytes.front() == 6 && bytes.back() == 6); // In-flight pre-seek data was discarded.
    assert(ahead.read(bytes.data(), 32, 0, {}) == 32 && bytes[0] == 0); // Backward seek.
    ahead.close();
    std::puts("Read-ahead: cached playback during disk stall, cancellation and seek passed");
}

int main(int argc, char** argv) {
    assert(argc == 2);
    av_log_set_level(AV_LOG_ERROR);
    tail_windows(); read_ahead_stall(); raw_reads(); video_buffer_window(); completed_snapshot(); large_reads(); faststart(argv[1]); large_mp4(); growing_mp4(); faststart(argv[1], false); tail_metadata("tail.mp4"); seek_target(argv[1]);
    assert(MediaLoadView::make(0, 262144, false, false).percent == -1);
    assert(MediaLoadView::make(1024, 2048, false, false).percent == 99);
    assert(MediaLoadView::make(1024, 1024, true, false).waiting); // Download complete; decoder still opening.
    std::puts("Readable-prefix bounds, cancellation, seek bounds and EOF passed");
}
