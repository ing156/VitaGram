#include "download_avio.h"
#include "media_load_view.h"
#include "media_seek.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <future>
#include <thread>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
}
using namespace std::chrono_literals;

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
    format->interrupt_callback = {cancelled, &stop};
    DownloadAvio io;
    assert(io.attach(format, state) == 1);
    assert(download_stream::reading());
    uint8_t buffer[64]{};
    // Use callbacks directly so AVIO buffering cannot conceal a read of a gap.
    assert(format->pb->read_packet(format->pb->opaque, buffer, 64) == 16);
    auto blocked = std::async(std::launch::async, [&] { return format->pb->read_packet(format->pb->opaque, buffer, 64); });
    assert(blocked.wait_for(50ms) == std::future_status::timeout);
    stop = true;
    assert(blocked.wait_for(500ms) == std::future_status::ready);
    assert(blocked.get() == AVERROR_EXIT);
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

// Use FFmpeg's MP4 muxer to make a front-moov variant of the real H.264/AAC fixture.
static void faststart(const char* source) {
    AVFormatContext *in = nullptr, *out = nullptr;
    assert(avformat_open_input(&in, source, nullptr, nullptr) >= 0);
    assert(avformat_alloc_output_context2(&out, nullptr, "mp4", "fast.mp4") >= 0);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        auto* stream = avformat_new_stream(out, nullptr);
        assert(stream && avcodec_parameters_copy(stream->codecpar, in->streams[i]->codecpar) >= 0);
        stream->codecpar->codec_tag = 0;
        stream->time_base = in->streams[i]->time_base;
    }
    assert(avio_open(&out->pb, "fast.mp4", AVIO_FLAG_WRITE) >= 0);
    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "faststart", 0);
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
}

static int decode(download_stream::Shared state, AVMediaType type, std::atomic<bool>& first, std::atomic<bool>* opened = nullptr) {
    auto* format = avformat_alloc_context();
    DownloadAvio io;
    assert(io.attach(format, state) == 1);
    format->probesize = 256 * 1024;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist", "mov,mp4,m4a,3gp,3g2,mj2", 0);
    assert(avformat_open_input(&format, state->source.c_str(), nullptr, &options) >= 0);
    av_dict_free(&options);
    int index = av_find_best_stream(format, type, -1, -1, nullptr, 0);
    assert(index >= 0);
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
    assert(video_frames > 0 && video_frames == decode(complete, AVMEDIA_TYPE_VIDEO, video));
    assert(audio_frames > 0 && audio_frames == decode(complete, AVMEDIA_TYPE_AUDIO, audio));
    assert(!download_stream::reading());
    std::printf("Progressive MP4: %d video frames, %d audio frames; early start, starvation, rename and complete-file parity passed\n", video_frames, audio_frames);
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
    state->update(path, size, 0, size, true);
    assert(job.wait_for(20s) == std::future_status::ready && job.get() > 0);
    assert(first && !download_stream::reading());
    const auto completed_view = MediaLoadView::make(size, size, true, opened);
    assert(completed_view.controls && !completed_view.downloading && !completed_view.waiting);
    std::puts("MP4 tail metadata: waits for missing header bytes and decodes after completion");
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

int main(int argc, char** argv) {
    assert(argc == 2);
    av_log_set_level(AV_LOG_ERROR);
    raw_reads(); faststart(argv[1]); growing_mp4(); tail_metadata(argv[1]); seek_target(argv[1]);
    assert(MediaLoadView::make(0, 262144, false, false).percent == -1);
    assert(MediaLoadView::make(1024, 2048, false, false).percent == 99);
    assert(MediaLoadView::make(1024, 1024, true, false).waiting); // Download complete; decoder still opening.
    std::puts("Readable-prefix bounds, cancellation, seek bounds and EOF passed");
}
