#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <pthread.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#else
#include <chrono>
#endif

#ifndef VITAGRAM_PERFORMANCE
#define VITAGRAM_PERFORMANCE 1
#endif

// Fixed-size, allocation-free collection. Producers never wait for diagnostics.
namespace performance {
constexpr bool enabled = VITAGRAM_PERFORMANCE != 0;
// Bytes of CDRAM each owner holds, logged beside free_cdram to tell cache fill
// from a leak. Written by owners, read by the log writer.
inline std::atomic<int64_t> cdram_images{0}, cdram_glyphs{0}, cdram_video{0};
inline int64_t now() {
    if (!enabled) return 0;
#ifdef __vita__
    return sceKernelGetSystemTimeWide();
#else
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
enum class Metric {
    Frame, UiWork, InputSubmit, GpuWait, Swap, ImageDecode, ImageUpload, EmojiDecode,
    Pump, Process, PublishChats, RebuildHistory, Parse, RawWait, EventWait,
    RequestWait, ReceiveGap, Receive, Send, QueueLock,
    MediaResolve, MediaLock, MediaFlush, UiRemember, UiLock, UiFlush,
    AudioCancel, AudioStop, AudioEncode, VideoCancel, VideoEncode,
    PlayerStop, QrStop, FontsInit, NetworkInit, CacheRestore, StartupUi, StartupReady, StartupLiveList, StartupLiveReceived, StartupLiveQueue,
    UiSnapshot, UiWrite,
    TdActor, TdPoll, TdSocketRead, TdSocketWrite, TdFactorize, TdDhCheck, TdDns,
    TdFileIo, TdLogWrite, TdSocketConnect, MediaValidate, CacheApply,
    CacheRead, CacheParse, CacheMerge, CacheStartupSnapshot, CacheIndexRead, CacheDetailsLoad, CachePause, CacheService, JsonRelease, EventSelect, FirstHistoryRequestWait, FirstHistoryRawWait, FirstHistoryUiWait, FirstHistoryApply, FirstHistoryReady, ChatFirstPresent, ChatFirstLivePresent, FirstHistorySend, FirstHistoryResponse, FirstHistoryParse, ForumRequestWait, ForumSend, ForumResponse, ForumReady, TdFileSync, TdFileMeta, FontsFallback, FontsEmoji, CacheHistory,
    DirProfilePhotos, DirThumbnails, DirPhotos, DirStickers, DirTemp, MediaRotate, TrashRemove, FolderReady, ReactionResponse, VideoReady, StickerCompactMiss, MessagesCompactHit, MessagesCompactMiss, MessagesCompactUs, MessagesRememberUs, TdFileMetaSticker, TdFileMetaOther, TdFileMetaTemp, TdFileMetaPhotos, TdFileMetaThumbs, TdFileMetaDb, FolderFirstRow, FolderRequests,
    VideoCaptureRead, VideoCaptureCrop, VideoCapturePreview, VideoCaptureWrite, VideoCaptureGpuWait,
    VideoCaptureQueuePeak, AudioCaptureQueuePeak, VideoCaptureDropped,
    WebRunLoop, WebPaint, WebBridgeSubmit, WebBridgeReply, WebBridgeLatency, FrameTail, TextPoll,
    ParsedWait, ParsedOldestAge, RawOldestAge, PumpBudget, PumpBatch, PumpOverrun, StreamRead, StreamWait, VideoDownloadRate, VideoPrefixRate, Count
};
constexpr const char* names[] = {
    "frame_us", "ui_work_us", "input_submit_us", "gpu_wait_us", "swap_us", "image_decode_us", "image_upload_us", "emoji_decode_us",
    "pump_us", "process_us", "publish_chats_us", "rebuild_history_us", "parse_us", "raw_wait_us", "event_wait_us",
    "request_wait_us", "receive_gap_us", "receive_us", "send_us", "queue_lock_us",
    "media_resolve_us", "media_lock_us", "media_flush_us", "ui_remember_us", "ui_lock_us", "ui_flush_us",
    "audio_cancel_us", "audio_stop_us", "audio_encode_us", "video_cancel_us", "video_encode_us",
    "player_stop_us", "qr_stop_us", "fonts_init_us", "network_init_us", "cache_restore_us", "startup_ui_us", "startup_ready_us", "startup_live_list_us", "startup_live_received_us", "startup_live_queue_us",
    "ui_snapshot_us", "ui_write_us",
    "td_actor_us", "td_poll_us", "td_socket_read_us", "td_socket_write_us", "td_factorize_us", "td_dh_check_us", "td_dns_us",
    "td_file_io_us", "td_log_write_us", "td_socket_connect_us", "media_validate_us", "cache_apply_us",
    "cache_read_us", "cache_parse_us", "cache_merge_us", "cache_startup_snapshot_us", "cache_index_read_us", "cache_details_load_us", "cache_pause_us", "cache_service_us", "json_release_us", "event_select_us", "first_history_request_wait_us", "first_history_raw_wait_us", "first_history_ui_wait_us", "first_history_apply_us", "first_history_ready_us", "chat_first_present_us", "chat_first_live_present_us", "first_history_send_us", "first_history_response_us", "first_history_parse_us", "forum_request_wait_us", "forum_send_us", "forum_response_us", "forum_ready_us", "td_file_sync_us", "td_file_meta_us", "fonts_fallback_us", "fonts_emoji_us", "cache_history_us",
    "dir_profile_photos_entries", "dir_thumbnails_entries", "dir_photos_entries", "dir_stickers_entries", "dir_temp_entries",
    "media_rotate_us", "trash_remove_us", "folder_ready_us", "reaction_response_us", "video_ready_us", "sticker_compact_miss", "messages_compact_hit", "messages_compact_miss", "messages_compact_us", "messages_remember_us", "td_file_meta_sticker_us", "td_file_meta_other_us", "td_file_meta_temp_us", "td_file_meta_photos_us", "td_file_meta_thumbs_us", "td_file_meta_db_us", "folder_first_row_us", "folder_requests",
    "video_capture_read_us", "video_capture_crop_us", "video_capture_preview_us", "video_capture_write_us", "video_capture_gpu_wait_us",
    "video_capture_queue_peak_frames", "audio_capture_queue_peak_blocks", "video_capture_dropped_frames",
    "web_runloop_us", "web_paint_us", "web_bridge_submit_us", "web_bridge_reply_us", "web_bridge_latency_us", "frame_tail_us", "text_poll_us",
    "parsed_wait_us", "parsed_oldest_age_us", "raw_oldest_age_us", "pump_budget_us", "pump_batch_events", "pump_overrun_us", "stream_read_us", "stream_wait_us", "video_download_bytes_per_s", "video_prefix_bytes_per_s"
};
constexpr size_t metric_count = static_cast<size_t>(Metric::Count);
static_assert(sizeof(names)/sizeof(*names) == metric_count, "Metric names");
constexpr std::array<uint64_t, 27> bounds{{0, 100, 250, 500, 1000, 2000, 4000, 8000,
    12000, 16000, 16667, 20000, 25000, 33334, 50000, 75000, 100000, 150000,
    250000, 500000, 1000000, 2000000, 5000000, 10000000, 30000000, 60000000, UINT64_MAX}};
struct Histogram {
    uint64_t count = 0, sum = 0, maximum = 0;
    std::array<uint64_t, bounds.size()> buckets{};
    void add(uint64_t value) {
        ++count; sum += value; maximum = std::max(maximum, value);
        ++buckets[std::lower_bound(bounds.begin(), bounds.end(), value) - bounds.begin()];
    }
    // Conservative upper bucket bound; never label this as an exact percentile.
    uint64_t percentile(unsigned percent) const {
        if (!count) return 0;
        uint64_t target = (count * std::min(100u, std::max(1u, percent)) + 99) / 100, seen = 0;
        for (size_t i = 0; i < buckets.size(); ++i) {
            seen += buckets[i];
            if (seen >= target) return std::min(bounds[i], maximum);
        }
        return maximum;
    }
};
enum class Gauge { IncomingCount, IncomingBytes, ParsedCount, ParsedWireBytes, OutgoingCount, Count };
constexpr size_t gauge_count = static_cast<size_t>(Gauge::Count);
constexpr const char* gauge_names[] = {"incoming_count", "incoming_bytes", "parsed_count", "parsed_wire_bytes", "outgoing_count"};
struct Level { uint64_t current = 0, peak = 0; };
enum class Screen { Unknown, Auth, Chats, History, Post, Photo, Video, WebApp, Settings, Topics, Search, Stickers, Gifs, Reactions, Upload, Recorder };
inline const char* screen_name(Screen screen) {
    constexpr const char* labels[] = {"unknown", "auth", "chats", "history", "post", "photo", "video", "webapp", "settings", "topics", "search", "stickers", "gifs", "reactions", "upload", "recorder"};
    const auto i = static_cast<unsigned>(screen);
    return i < sizeof(labels)/sizeof(*labels) ? labels[i] : "unknown";
}
struct FrameContext {
    uint64_t frame = 0;
    Screen screen = Screen::Unknown;
    bool combined = false, keyboard = false, scrolling = false, fullscreen = false;
    int rotation = 0, web_width = 0, web_height = 0;
};
// UI context stays on its owning thread; parser/network events retain frame=0.
inline thread_local FrameContext frame_context;
enum class Counter { Received, Parsed, Processed, PumpEmpty, PumpFiltered, PumpBudgetStop, PumpLimitStop, FrameOver33, FrameOver50, FrameOver100, Count };
constexpr size_t counter_count = static_cast<size_t>(Counter::Count);
constexpr const char* counter_names[] = {"received_events", "parsed_events", "processed_events", "pump_empty", "pump_filtered", "pump_budget_stop", "pump_limit_stop", "frames_over_33ms", "frames_over_50ms", "frames_over_100ms"};
static_assert(sizeof(counter_names)/sizeof(*counter_names) == counter_count, "Counter names");
enum class SlowPhase { Parse, Process, Remember, Copy, Retain, PollScan, Delete, Ui, Release, TdFileMeta, TdFileSync, Storage };
constexpr const char* slow_names[] = {"parse", "process", "remember", "copy", "retain", "poll_scan", "delete", "ui", "release", "td_file_meta", "td_file_sync", "storage"};
struct SlowEvent {
    SlowPhase phase = SlowPhase::Parse;
    std::array<char,64> type{};
    std::array<char,80> file_dir{};
    int file_fd = -1;
    int64_t file_offset = -1;
    uint64_t file_bytes = 0;
    int64_t elapsed = 0, at = 0;
    uint64_t wire_bytes = 0, chats = 0, histories = 0;
    int cpu_start = -1, cpu_end = -1;
    FrameContext context;
};
constexpr size_t slow_limit = 32;
struct Snapshot {
    int64_t from = 0, until = 0;
    uint64_t dropped = 0;
    std::array<Histogram, metric_count> metrics{};
    std::array<Level, gauge_count> gauges{};
    std::array<SlowEvent, slow_limit> slow{};
    size_t slow_count = 0;
    uint64_t slow_dropped = 0;
    std::array<uint64_t, counter_count> counters{};
    const Histogram& get(Metric m) const { return metrics[static_cast<size_t>(m)]; }
};
class Collector {
public:
    Collector() = default;
    Collector(const Collector&) = delete;
    ~Collector() { pthread_mutex_destroy(&mutex_); }
    void observe(Metric m, int64_t us) {
        if (!enabled || us < 0) return;
        if (pthread_mutex_trylock(&mutex_)) { dropped_.fetch_add(1, std::memory_order_relaxed); return; }
        window_.metrics[static_cast<size_t>(m)].add(static_cast<uint64_t>(us));
        if (m == Metric::Frame) {
            if (us > 33334) ++window_.counters[static_cast<size_t>(Counter::FrameOver33)];
            if (us > 50000) ++window_.counters[static_cast<size_t>(Counter::FrameOver50)];
            if (us > 100000) ++window_.counters[static_cast<size_t>(Counter::FrameOver100)];
        }
        pthread_mutex_unlock(&mutex_);
    }
    // One-shot values (startup milestones) must not be lost to the trylock under load.
    void observeAlways(Metric m, int64_t us) {
        if (!enabled || us < 0) return;
        pthread_mutex_lock(&mutex_);
        window_.metrics[static_cast<size_t>(m)].add(static_cast<uint64_t>(us));
        pthread_mutex_unlock(&mutex_);
    }
    void queues(uint64_t incoming, uint64_t bytes, uint64_t parsed, uint64_t wire_bytes, uint64_t outgoing) {
        if (!enabled) return;
        if (pthread_mutex_trylock(&mutex_)) { dropped_.fetch_add(1, std::memory_order_relaxed); return; }
        const uint64_t values[] = {incoming, bytes, parsed, wire_bytes, outgoing};
        for (size_t i = 0; i < gauge_count; ++i) {
            window_.gauges[i].current = values[i];
            window_.gauges[i].peak = std::max(window_.gauges[i].peak, values[i]);
        }
        pthread_mutex_unlock(&mutex_);
    }
    void count(Counter counter, uint64_t amount = 1) {
        if (!enabled) return;
        if (pthread_mutex_trylock(&mutex_)) { dropped_.fetch_add(1, std::memory_order_relaxed); return; }
        window_.counters[static_cast<size_t>(counter)] += amount;
        pthread_mutex_unlock(&mutex_);
    }
    void slow(const SlowEvent& event) {
        if (!enabled || event.elapsed < 50000) return;
        if (pthread_mutex_trylock(&mutex_)) { dropped_.fetch_add(1,std::memory_order_relaxed); return; }
        if (window_.slow_count < slow_limit) window_.slow[window_.slow_count++]=event;
        else {
            // Keep specific causes before their enclosing frame/draw totals.
            auto less = [](const SlowEvent& a, const SlowEvent& b) {
                auto aggregate = [](const SlowEvent& e) { return e.phase == SlowPhase::Ui &&
                    (!std::strcmp(e.type.data(), "frame_ui") || !std::strcmp(e.type.data(), "draw")); };
                if (aggregate(a) != aggregate(b)) return aggregate(a);
                return a.elapsed < b.elapsed;
            };
            auto smallest=std::min_element(window_.slow.begin(),window_.slow.end(),less);
            if (less(*smallest,event)) *smallest=event;
            ++window_.slow_dropped;
        }
        pthread_mutex_unlock(&mutex_);
    }
    // Only the background writer takes snapshots. No formatting/I/O under lock.
    Snapshot take(int64_t end) {
        Snapshot result;
        pthread_mutex_lock(&mutex_);
        result = window_; result.until = end;
        result.dropped = dropped_.exchange(0, std::memory_order_relaxed);
        window_.metrics = {}; window_.from = end;
        window_.slow_count=0; window_.slow_dropped=0; window_.counters={};
        for (auto& gauge : window_.gauges) gauge.peak = gauge.current;
        pthread_mutex_unlock(&mutex_);
        return result;
    }
    void start(int64_t start) {
        pthread_mutex_lock(&mutex_); window_.from = start; pthread_mutex_unlock(&mutex_);
    }
private:
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::atomic<uint32_t> dropped_{0};
    Snapshot window_;
};
inline Collector& collector() { static Collector instance; return instance; }
// Application start on the now() clock; startup metrics from other threads measure from it.
inline std::atomic<int64_t>& startup_origin() { static std::atomic<int64_t> origin{0}; return origin; }
inline void observe(Metric m, int64_t us) { if (enabled) collector().observe(m, us); }
inline void count(Counter counter, uint64_t amount = 1) { if (enabled) collector().count(counter, amount); }
inline void observeAlways(Metric m, int64_t us) { if (enabled) collector().observeAlways(m, us); }
class Scope {
public:
    explicit Scope(Metric m) : metric_(m), start_(enabled ? now() : 0) {}
    ~Scope() { finish(); }
    void finish() { if (enabled && start_) { observe(metric_, now()-start_); start_ = 0; } }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Metric metric_; int64_t start_;
};
inline int cpu() {
#ifdef __vita__
    return enabled ? sceKernelGetCpuId() : -1;
#else
    return -1;
#endif
}
// Copies schema names while the JSON is alive; never retains payload pointers.
class SlowScope {
public:
    SlowScope(SlowPhase phase,const char* type,uint64_t bytes=0) : start_(now()) {
        event_.phase=phase; event_.wire_bytes=bytes; event_.cpu_start=cpu(); name(type);
    }
    ~SlowScope() { finish(); }
    void name(const char* type) {
        if (!enabled || !type) return;
        size_t i=0;
        for (;i+1<event_.type.size() && type[i];++i) {
            unsigned char c=type[i];
            event_.type[i]=((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_') ? char(c) : '_';
        }
        event_.type[i]=0;
    }
    void next(const char* type) {
        finish(); start_ = now(); event_.cpu_start = cpu(); name(type);
    }
    void counts(uint64_t chats,uint64_t histories) { event_.chats=chats;event_.histories=histories; }
    void since(int64_t start) { if (start_) start_=start; } // Report an interval that began earlier.
    void finish() {
        if (!enabled || !start_) return;
        event_.at=now();event_.elapsed=event_.at-start_;start_=0;
        if (event_.elapsed<50000) return;
        event_.cpu_end=cpu();event_.context=frame_context;collector().slow(event_);
    }
    SlowScope(const SlowScope&)=delete;
    SlowScope& operator=(const SlowScope&)=delete;
private:
    SlowEvent event_{}; int64_t start_;
};
// Measures wait only, leaving ownership and unlock policy with the caller.
inline void lock(pthread_mutex_t* mutex, Metric metric) {
    int64_t started = enabled ? now() : 0;
    pthread_mutex_lock(mutex);
    if (enabled) observe(metric, now()-started);
}
}
