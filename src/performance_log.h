#pragma once
#include "performance.h"
#include "td_diagnostics.h"
#include "stream_diagnostics.h"
#include "video_diagnostics.h"
#include "ui_diagnostics.h"
#include "event_stats.h"
#include "audio_startup_trace.h"
#include <cstdio>
#include <sys/stat.h>
#ifdef __vita__
#include <psp2/power.h>
#include <psp2/kernel/sysmem.h>
#include <malloc.h>
extern "C" unsigned int _get_vita_heap_size(void);
#include "cpu_scheduler.h"
#endif

namespace performance {
inline void writeSnapshot(FILE* file, const Snapshot& snapshot, const char* version) {
    std::fprintf(file, "perf version=%s from_us=%lld until_us=%lld dropped_samples=%llu\n", version,
        (long long)snapshot.from, (long long)snapshot.until, (unsigned long long)snapshot.dropped);
    std::fprintf(file, "session_us=%lld dropped_samples_kind=diagnostic_measurements\n", (long long)startup_origin().load());
    for (size_t i = 0; i < metric_count; ++i) {
        const auto& h = snapshot.metrics[i];
        if (!h.count) continue;
        std::fprintf(file, "%s n=%llu mean=%llu p50_le=%llu p95_le=%llu p99_le=%llu max=%llu\n", names[i],
            (unsigned long long)h.count, (unsigned long long)(h.sum/h.count),
            (unsigned long long)h.percentile(50), (unsigned long long)h.percentile(95),
            (unsigned long long)h.percentile(99), (unsigned long long)h.maximum);
    }
    for (size_t i = 0; i < counter_count; ++i)
        std::fprintf(file, "counter name=%s value=%llu\n", counter_names[i], (unsigned long long)snapshot.counters[i]);
    for (size_t i = 0; i < gauge_count; ++i)
        std::fprintf(file, "%s current=%llu peak=%llu\n", gauge_names[i],
            (unsigned long long)snapshot.gauges[i].current, (unsigned long long)snapshot.gauges[i].peak);
    if (snapshot.slow_count || snapshot.slow_dropped)
        std::fprintf(file,"slow_events threshold_us=50000 kept=%llu omitted=%llu\n",(unsigned long long)snapshot.slow_count,(unsigned long long)snapshot.slow_dropped);
    for (size_t i=0;i<snapshot.slow_count;++i) {
        const auto& e=snapshot.slow[i];
        std::fprintf(file,"slow_event phase=%s type=%s elapsed_us=%lld at_us=%lld wire_bytes=%llu chats=%llu histories=%llu cpu_start=%d cpu_end=%d file_dir=%s file_fd=%d file_offset=%lld file_bytes=%llu frame=%llu screen=%s combined=%d keyboard=%d scrolling=%d fullscreen=%d rotation=%d web_width=%d web_height=%d\n",
            slow_names[static_cast<size_t>(e.phase)],e.type.data(),(long long)e.elapsed,(long long)e.at,
            (unsigned long long)e.wire_bytes,(unsigned long long)e.chats,(unsigned long long)e.histories,e.cpu_start,e.cpu_end,
            e.file_dir[0] ? e.file_dir.data() : "unknown", e.file_fd, (long long)e.file_offset, (unsigned long long)e.file_bytes,
            (unsigned long long)e.context.frame,screen_name(e.context.screen),e.context.combined,e.context.keyboard,
            e.context.scrolling,e.context.fullscreen,e.context.rotation,e.context.web_width,e.context.web_height);
    }
}
// Exactly one background cache worker calls this, at most once per 5 seconds.
// The current interval is deliberately not retained on disk errors: RAM stays bounded.
inline bool flush(const char* path, const char* previous, const char* version, int64_t time) {
    if (!enabled) return true;
    auto snapshot = collector().take(time);
    struct stat info{};
    bool ok = true;
    if (!stat(path, &info) && info.st_size >= 1024*1024) ok = std::rename(path, previous) == 0;
    if (ok) {
        FILE* file = std::fopen(path, "ab");
        if (file) {
            writeSnapshot(file, snapshot, version);
            audio_startup_trace::write(file);
            td_diagnostics::write(file);
            stream_diagnostics::write(file);
            video_diagnostics::write(file);
            ui_diagnostics::write(file);
            event_stats::write(file);
#ifdef __vita__
            SceKernelFreeMemorySizeInfo memory{}; memory.size = sizeof(memory);
            const int memory_result = sceKernelGetFreeMemorySize(&memory);
            const struct mallinfo heap = mallinfo(); // newlib heap is a fixed block; free_user does not see it
            std::fprintf(file, "device cpu_mhz=%d bus_mhz=%d gpu_mhz=%d battery_temp_centi_c=%d memory_result=%d free_user=%d free_cdram=%d free_phycont=%d heap_used=%d heap_free=%d cdram_images=%lld cdram_glyphs=%lld cdram_video=%lld\n",
                scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(),
                scePowerGetBatteryTemp(), memory_result, memory.size_user, memory.size_cdram, memory.size_phycont, (int)heap.uordblks, (int)(_get_vita_heap_size() - heap.uordblks),
                (long long)cdram_images.load(), (long long)cdram_glyphs.load(), (long long)cdram_video.load());
            cpu_scheduler::writeReport(file);
#endif
            ok = !std::ferror(file);
            if (std::fclose(file)) ok = false;
        } else ok = false;
    }
    return ok;
}
}
