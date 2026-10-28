#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <utility>
#include <cstring>
#include <string>
#include <vector>
#include <pthread.h>
#include <ctime>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/devctl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace storage_probe {
constexpr int block = 64 * 1024, blocks = 64;
constexpr int64_t reserve = 512LL * 1024 * 1024, file_bytes = 2LL * block * blocks;
inline int64_t now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline int io(int fd, void* data, int size, int64_t offset, bool write) {
#ifdef __vita__
    return write ? sceIoPwrite(fd, data, size, offset) : sceIoPread(fd, data, size, offset);
#else
    return write ? ::pwrite(fd, data, size, offset) : ::pread(fd, data, size, offset);
#endif
}
inline int remove(const char* path) {
#ifdef __vita__
    return sceIoRemove(path);
#else
    return ::unlink(path);
#endif
}
struct Samples {
    std::vector<int64_t> values;
    void print(FILE* log, const char* name) {
        std::sort(values.begin(), values.end());
        int64_t sum = 0; for (auto v : values) sum += v;
        std::fprintf(log, "%s n=%u mean_us=%lld p95_us=%lld max_us=%lld\n", name, unsigned(values.size()),
            (long long)(values.empty() ? 0 : sum / values.size()),
            (long long)(values.empty() ? 0 : values[(values.size() * 95 - 1) / 100]),
            (long long)(values.empty() ? 0 : values.back()));
    }
};
struct Job {
    int fd;
    bool (*proceed)(void*);
    void* context;
    int64_t deadline;
    std::atomic<bool> start{false}, stop{false}, ready{false};
    int64_t write_begin = 0, write_end = 0;
    Samples writes;
    int error = 0;
    Job(int f, bool (*p)(void*), void* c) : fd(f), proceed(p), context(c), deadline(now() + 30000000) {}
    bool allowed() { return !stop && now() < deadline && proceed(context); }
    static void* writer(void* opaque) {
        auto& job = *static_cast<Job*>(opaque);
        job.ready = true;
        while (!job.start) {
            if (!job.allowed()) return nullptr;
            struct timespec delay{0, 1000000}; nanosleep(&delay, nullptr);
        }
        std::vector<unsigned char> data(block, 0xa5);
        job.write_begin = now();
        for (int i = 0; i < blocks && job.allowed(); ++i) {
            const auto begin = now();
            const int rc = io(job.fd, data.data(), block, int64_t(blocks + i) * block, true);
            job.writes.values.push_back(now() - begin);
            if (rc != block) { job.error = rc; job.stop = true; break; }
        }
        job.write_end = now();
        return nullptr;
    }
};
// Exclusive creation prevents a probe from overwriting any existing file, including a stale probe.
inline void run(const std::string& directory, int64_t free_bytes, FILE* log, bool (*proceed)(void*), void* context) {
    std::fprintf(log, "storage path=%s free_bytes=%lld reserve_bytes=%lld\n", directory.c_str(), (long long)free_bytes, (long long)reserve);
    if (free_bytes < reserve + file_bytes) { std::fprintf(log, "result=skip_space\n"); return; }
    if (!proceed(context)) { std::fprintf(log, "result=cancelled\n"); return; }
    const auto path = directory + "/vitagram-storage-probe.tmp";
#ifdef __vita__
    const int fd = sceIoOpen(path.c_str(), SCE_O_RDWR | SCE_O_CREAT | SCE_O_EXCL, 0666);
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
#endif
    if (fd < 0) { std::fprintf(log, "result=open_failed code=%d\n", fd); return; }
    Job job{fd, proceed, context};
    std::vector<unsigned char> data(block, 0xa5);
    Samples preparation, reads;
    std::vector<std::pair<int64_t, int64_t>> read_times;
    bool ok = true;
    for (int i = 0; i < blocks; ++i) {
        if (!job.allowed()) { ok = false; break; }
        const auto begin = now();
        const int rc = io(fd, data.data(), block, int64_t(i) * block, true);
        preparation.values.push_back(now() - begin);
        if (rc != block) { std::fprintf(log, "prepare_error=%d\n", rc); ok = false; break; }
    }
#ifdef __vita__
    const int reader = ok ? sceIoOpen(path.c_str(), SCE_O_RDONLY, 0) : -1;
#else
    const int reader = ok ? ::open(path.c_str(), O_RDONLY) : -1;
#endif
    if (reader < 0) ok = false;
    pthread_t thread{};
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 128 * 1024);
    const int started = ok ? pthread_create(&thread, &attr, Job::writer, &job) : -1;
    pthread_attr_destroy(&attr);
    if (started == 0) {
        while (!job.ready && job.allowed()) {
            struct timespec delay{0, 1000000}; nanosleep(&delay, nullptr);
        }
        job.start = true;
        for (int i = 0; i < blocks && job.allowed(); ++i) {
            const auto begin = now();
            const int rc = io(reader, data.data(), block, int64_t(i) * block, false);
            const auto end = now();
            reads.values.push_back(end - begin);
            read_times.emplace_back(begin, end);
            if (rc != block || !std::all_of(data.begin(), data.end(), [](unsigned char c) { return c == 0xa5; })) {
                std::fprintf(log, "read_error=%d\n", rc); ok = false; job.stop = true; break;
            }
        }
        pthread_join(thread, nullptr);
    } else ok = false;
#ifdef __vita__
    if (reader >= 0) sceIoClose(reader);
    const int closed = sceIoClose(fd);
#else
    if (reader >= 0) ::close(reader);
    const int closed = ::close(fd);
#endif
    const int removed = remove(path.c_str());
    unsigned overlap = 0;
    for (auto interval : read_times)
        if (interval.first < job.write_end && interval.second > job.write_begin) ++overlap;
    std::fprintf(log, "overlap_read_samples=%u\n", overlap);
    preparation.print(log, "prepare_write_64k"); reads.print(log, "concurrent_read_64k"); job.writes.print(log, "concurrent_write_64k");
    std::fprintf(log, "result=%s writer_error=%d close=%d cleanup=%d elapsed_limit_s=30\n",
        ok && reads.values.size() == blocks && job.writes.values.size() == blocks && !job.stop && closed == 0 && removed == 0 ? "ok" : "incomplete",
        job.error, closed, removed);
}
#ifdef __vita__
inline void poll(bool (*proceed)(void*), void* context, const char* version) {
    const char* request = "ux0:data/vitagram/storage-test.request";
    SceIoStat stat{};
    if (!proceed(context) || sceIoGetstat(request, &stat) < 0) return;
    FILE* log = std::fopen("ux0:data/vitagram/storage-test.log", "wb");
    if (!log) return;
    if (remove(request) < 0) { std::fclose(log); return; }
    std::fprintf(log, "storage_probe version=%s block_bytes=%d samples=%d max_file_bytes=%lld physical_device=unknown\n", version, block, blocks, (long long)file_bytes);
    for (const char* mount : {"ux0:", "ur0:", "uma0:"}) {
        SceIoDevInfo info{};
        const int rc = sceIoDevctl(mount, 0x3001, nullptr, 0, &info, sizeof(info));
        std::fprintf(log, "mount=%s devctl=%d total_bytes=%lld\n", mount, rc, (long long)info.max_size);
        if (rc < 0) { std::fprintf(log, "result=unavailable\n"); continue; }
        const std::string directory = std::string(mount) + "data";
        sceIoMkdir(directory.c_str(), 0777);
        run(directory, info.free_size, log, proceed, context);
    }
    std::fclose(log);
}
#endif
}
