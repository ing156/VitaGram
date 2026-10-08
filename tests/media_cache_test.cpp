#include "media_cache.h"
#include "config.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

static constexpr const char* path = VITAGRAM_FILES_DIR "/checked.bin";
int sceKernelDelayThread(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); return 0; }
static std::atomic<int> checks{0};
static std::atomic<bool> blocked{false}, entered{false}, released{false};
extern "C" int __real_stat(const char*, struct stat*);
extern "C" int __wrap_stat(const char* name, struct stat* out) {
    if (!std::strcmp(name, path)) {
        ++checks;
        if (blocked) {
            entered = true;
            while (!released) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return __real_stat(name, out);
}
static json_t* file() {
    return json_pack("{s:s,s:i,s:i,s:{s:s},s:{s:b,s:s}}", "@type", "file", "id", 10, "size", 4,
        "remote", "unique_id", "unique", "local", "is_downloading_completed", true, "path", path);
}
int main() {
    mkdir("ux0:data", 0777); mkdir("ux0:data/vitagram", 0777);
    mkdir(VITAGRAM_DB_DIR, 0777); mkdir(VITAGRAM_FILES_DIR, 0777);
    FILE* f = std::fopen(path, "wb"); assert(f); std::fputs("data", f); std::fclose(f);
    {
        MediaCache priority;
        for (int id=1;id<=6;++id) {auto* value=file();json_object_set_new(value,"id",json_integer(id));priority.enqueue(value);json_decref(value);if (id>=2) priority.prioritize(id);}
        for (int expected:{2,3,4,1,5,6}) {
            assert(priority.validateNext());auto* value=priority.takeValidated();assert(value);
            assert(json_integer_value(json_object_get(value,"id"))==expected);json_decref(value);
        }
        assert(!priority.validateNext());
        for (int id=1;id<=6;++id) {auto* value=file();json_object_set_new(value,"id",json_integer(id));priority.enqueue(value);json_decref(value);if (id>=2) priority.prioritize(id);}
        for (int i=0;i<6;++i) assert(priority.validateNext());
        for (int expected:{2,1,3,4,5,6}) {
            auto* value=priority.takeValidated();assert(value);assert(json_integer_value(json_object_get(value,"id"))==expected);json_decref(value);
        }
    }
    checks=0;
    MediaCache cache; cache.load();
    auto* many = json_array();
    for (int i = 0; i < 500; ++i) json_array_append_new(many, file());
    assert(cache.resolve(many)); assert(checks == 1); // one stat for duplicate references
    assert(cache.resolve(many, false)); assert(checks == 1); // UI has no disk I/O
    assert(cache.resolve(many)); assert(checks == 1); // recent result reused
    json_decref(many);

    cache.invalidate(); blocked = true;
    auto* root = file();
    auto worker = std::async(std::launch::async, [&] { return cache.resolve(root); });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(entered);
    auto ui = std::async(std::launch::async, [&] {
        auto* pending = file();
        cache.resolve(pending, false);
        assert(!json_is_true(json_object_get(json_object_get(pending, "local"), "is_downloading_completed")));
        json_decref(pending); cache.clear();
    });
    bool responsive = ui.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    released = true; ui.get();
    assert(responsive && !worker.get()); // clear cancels in-flight publication
    json_decref(root); blocked = false;
    auto* cleared = file();
    assert(cache.resolve(cleared, false));
    assert(!json_is_true(json_object_get(json_object_get(cleared, "local"), "is_downloading_completed")));
    json_decref(cleared);

    auto* valid = file(); assert(cache.resolve(valid)); json_decref(valid);
    assert(!unlink(path)); cache.invalidate();
    auto* missing = file(); assert(cache.resolve(missing));
    assert(!json_is_true(json_object_get(json_object_get(missing, "local"), "is_downloading_completed")));
    json_decref(missing);
    // Parser checks must not mutate an upload's original path/protocol flags.
    auto* original = file();
    assert(cache.resolve(original, true, nullptr, nullptr, false));
    assert(json_is_true(json_object_get(json_object_get(original, "local"), "is_downloading_completed")));
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(original, "local"), "path")), path));
    assert(cache.resolve(original, false));
    assert(!json_is_true(json_object_get(json_object_get(original, "local"), "is_downloading_completed")));
    json_decref(original);
    auto* cancelled = file(); const int before = checks;
    assert(!cache.resolve(cancelled, true, [](void*) { return false; }));
    assert(checks == before); json_decref(cancelled);
    // A blocked storage job must not block queuing a newer protocol state.
    f = std::fopen(path, "wb"); assert(f); std::fputs("data", f); std::fclose(f);
    cache.clear(); entered = false; released = false; blocked = true;
    auto* queued = file(); cache.enqueue(queued); json_decref(queued);
    auto validation = std::async(std::launch::async, [&] { return cache.validateNext(); });
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(entered);
    auto replacement = std::async(std::launch::async, [&] {
        auto* newer = file();
        json_object_set_new(newer, "size", json_integer(9)); // old size must not win
        cache.enqueue(newer); json_decref(newer);
        assert(!cache.takeValidated());
    });
    responsive = replacement.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    released = true; replacement.get(); assert(responsive && validation.get()); blocked = false;
    assert(!cache.takeValidated()); assert(cache.validateNext());
    auto* result = cache.takeValidated(); assert(result);
    assert(json_integer_value(json_object_get(result, "size")) == 9);
    assert(!json_is_true(json_object_get(json_object_get(result, "local"), "is_downloading_completed")));
    json_decref(result);

    cache.clear(); queued = file();
    for (int i = 0; i < 500; ++i) assert(cache.enqueue(queued).empty());
    const int prior = checks;
    assert(cache.validateNext()); assert(!cache.validateNext()); assert(checks == prior + 1);
    result = cache.takeValidated(); assert(result);
    assert(json_is_true(json_object_get(json_object_get(result, "local"), "is_downloading_completed")));
    json_decref(result);
    cache.enqueue(queued); assert(cache.validateNext());
    json_object_set_new(json_object_get(queued, "local"), "is_downloading_completed", json_false());
    // No cache fallback for a different remote identity: cancel ready old file.
    json_object_set_new(json_object_get(queued, "remote"), "unique_id", json_string("replacement"));
    cache.enqueue(queued); assert(!cache.takeValidated()); assert(!cache.validateNext());
    json_decref(queued);

    cache.clear(); entered = false; released = false; blocked = true;
    queued = file(); cache.enqueue(queued); json_decref(queued);
    validation = std::async(std::launch::async, [&] { return cache.validateNext(); });
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(entered); cache.invalidate(); released = true; assert(validation.get()); blocked = false;
    assert(!cache.takeValidated() && !cache.validateNext());

    // Overflow drops only optional file jobs, returning IDs for visible retries.
    cache.clear(); size_t displaced = 0;
    for (int i = 1; i <= 700; ++i) {
        queued = file(); json_object_set_new(queued, "id", json_integer(i));
        auto removed = cache.enqueue(queued); displaced += removed.size();
        if (i > 512) assert(removed.size() == 1 && removed[0] == i - 512);
        json_decref(queued);
    }
    assert(displaced == 188);
    int completed = 0;
    while (cache.validateNext()) ++completed;
    assert(completed == 512);
    completed = 0;
    while ((result = cache.takeValidated())) { ++completed; json_decref(result); }
    assert(completed == 512);

    // A fresh TDLib session reports an indexed avatar as not downloaded:
    // the download waits for the index check instead of storing a duplicate.
    cache.clear();
    auto* indexed = file(); assert(cache.resolve(indexed)); json_decref(indexed);
    auto* fresh = file(); json_object_set_new(fresh, "id", json_integer(77));
    json_object_set_new(json_object_get(fresh, "local"), "is_downloading_completed", json_false());
    json_object_set_new(json_object_get(fresh, "local"), "path", json_string(""));
    assert(!cache.awaiting(77)); cache.enqueue(fresh);
    assert(cache.awaiting(77));
    assert(cache.validateNext()); assert(!cache.awaiting(77));
    result = cache.takeValidated(); assert(result);
    assert(json_is_true(json_object_get(json_object_get(result, "local"), "is_downloading_completed")));
    json_decref(result);
    // Unknown files are downloaded at once.
    json_object_set_new(json_object_get(fresh, "remote"), "unique_id", json_string("never-seen"));
    cache.enqueue(fresh); assert(!cache.awaiting(77));
    json_decref(fresh);
    std::puts("Media cache: download waits only for an indexed copy under verification");
    std::puts("Async media: blocked storage, newer state, ready cancellation, invalidation and bounded overflow passed");
    std::puts("Media cache: deduplicated stat, no UI I/O, unlocked slow storage, cancellation and deleted files passed");
}
