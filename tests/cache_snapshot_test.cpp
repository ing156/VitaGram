#include <jansson.h>
#include <pthread.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <string>
#include <cassert>
#include <cstring>
#include <sys/stat.h>
static std::atomic<bool> load_blocked{false}, load_entered{false}, load_released{false};
extern "C" int __real_stat(const char*, struct stat*);
extern "C" int __wrap_stat(const char* path, struct stat* info) {
    if (load_blocked && !std::strcmp(path, "ux0:data/vitagram/db/ui-cache.json")) {
        load_entered = true;
        while (!load_released) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return __real_stat(path, info);
}
#define private public
#include "ui_cache.h"
#undef private
#include "cache_writer.h"
#include "exit_gesture.h"
#include "config.h"

int sceKernelDelayThread(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); return 0; }
static json_t* parse(const char* s) { json_error_t e{}; auto* v=json_loads(s,0,&e); assert(v); return v; }
static std::string dump(json_t* v) { char* s=json_dumps(v,JSON_COMPACT); assert(s); std::string r=s; free(s); return r; }
static void event(UiCache& c, const char* s) { auto* v=parse(s); c.remember(v); json_decref(v); }
static json_t* freeze(UiCache& c) {
    pthread_mutex_lock(&c.mutex_); auto* v=c.frozenSnapshot(); pthread_mutex_unlock(&c.mutex_); assert(v); return v;
}
int main() {
    mkdir("ux0:data",0777); mkdir("ux0:data/vitagram",0777); mkdir(VITAGRAM_DB_DIR,0777);
    UiCache c; c.load();
    event(c,R"({"@type":"chat","id":42,"title":"Before","type":{"@type":"chatTypePrivate","user_id":7},"positions":[{"list":{"@type":"chatListMain"},"order":12}],"photo":{"small":{"id":1}},"unneeded_blob":"discard"})");
    event(c,R"({"@type":"message","id":55,"chat_id":42,"content":{"@type":"messagePoll","poll":{"id":"77","is_closed":false}}})");
    auto* packs=parse(R"({"sets":[{"id":3}]})");
    c.saveStickerPacks(packs); c.saveStickerItems(3,packs);
    event(c,R"({"@type":"updateChatFolders","chat_folders":[{"id":1}]})");
    event(c,R"({"@type":"user","id":7,"first_name":"A","profile_photo":{"small":{"id":8}}})");
    json_t* frozen=freeze(c); const auto before=dump(frozen);
    assert(before.find("unneeded_blob")==std::string::npos);
    // Mutating source objects and every cached mutable branch cannot alter a snapshot.
    json_array_clear(json_object_get(packs,"sets")); json_decref(packs);
    event(c,R"({"@type":"updateChatTitle","chat_id":42,"title":"After"})");
    event(c,R"({"@type":"updateChatPosition","chat_id":42,"position":{"list":{"@type":"chatListMain"},"order":99}})");
    event(c,R"({"@type":"updateChatPhoto","chat_id":42,"photo":{"small":{"id":2}}})");
    event(c,R"({"@type":"updateChatDraftMessage","chat_id":42,"positions":[]})");
    event(c,R"({"@type":"updatePoll","poll":{"id":"77","is_closed":true}})");
    event(c,R"({"@type":"updateMessageEdited","chat_id":42,"message_id":55,"edit_date":99})");
    event(c,R"({"@type":"updateMessageInteractionInfo","chat_id":42,"message_id":55,"interaction_info":{"view_count":100}})");
    event(c,R"({"@type":"updateMessageContent","chat_id":42,"message_id":55,"new_content":{"@type":"messageText","text":{"text":"changed"}}})");
    event(c,R"({"@type":"updateChatFolders","chat_folders":[]})");
    event(c,R"({"@type":"user","id":7,"first_name":"B"})");
    c.clear(); assert(dump(frozen)==before); json_decref(frozen);

    // Block disk serialization after a snapshot has been taken. UI updates must
    // complete while the writer is stalled and must not leak into that snapshot.
    std::string large(200000,'x');
    auto* message=json_pack("{s:s,s:i,s:i,s:{s:s}}","@type","message","id",99,"chat_id",42,"content","text",large.c_str());
    c.remember(message); json_decref(message);
    event(c,R"({"@type":"chat","id":42,"title":"old"})");
    struct Gate { std::atomic<int> calls{0}; std::atomic<bool> waiting{false}, release{false}; } gate;
    auto proceed=[](void* p) -> bool {
        auto& g=*static_cast<Gate*>(p);
        if (++g.calls==3) {
            g.waiting=true;
            while (!g.release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    };
    auto writer=std::async(std::launch::async,[&]{ c.flush(proceed,&gate); });
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!gate.waiting && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(gate.waiting);
    auto update=std::async(std::launch::async,[&]{ event(c,R"({"@type":"updateChatTitle","chat_id":42,"title":"new"})"); });
    assert(update.wait_for(std::chrono::seconds(2))==std::future_status::ready); update.get();
    gate.release=true; writer.get();
    json_error_t error{};
    json_t* saved=json_load_file(VITAGRAM_DB_DIR "/ui-cache.json",0,&error); assert(saved);
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(json_object_get(saved,"chats"),"42"),"title")),"old"));
    json_decref(saved); assert(c.saved_!=c.revision_); c.flush(); assert(c.saved_==c.revision_);
    saved=json_load_file(VITAGRAM_DB_DIR "/ui-cache.json",0,&error); assert(saved);
    const auto saved_text=dump(saved); json_decref(saved);
    event(c,R"({"@type":"updateChatTitle","chat_id":42,"title":"interrupted"})");
    int calls=0;
    c.flush([](void* p){return ++*static_cast<int*>(p)<3;},&calls);
    assert(c.saved_!=c.revision_);
    saved=json_load_file(VITAGRAM_DB_DIR "/ui-cache.json",0,&error); assert(saved);
    assert(dump(saved)==saved_text); json_decref(saved);
    struct stat info{}; assert(stat(VITAGRAM_DB_DIR "/ui-cache.json.tmp",&info));
    auto* big=json_pack("{s:s}","text",large.c_str());
    assert(cache_writer::write(big,"bounded.tmp",100)==cache_writer::Result::Limit);
    assert(stat("bounded.tmp",&info)); json_decref(big);

    // Disk load racing with live updates: keep live records and don't resurrect
    // messages whose deletion arrived before their old history was read.
    UiCache early;
    event(early,R"({"@type":"chat","id":42,"title":"live","positions":[]})");
    event(early,R"({"@type":"user","id":7,"first_name":"live user"})");
    event(early,R"({"@type":"updateDeleteMessages","chat_id":42,"message_ids":[55]})");
    early.load();
    auto* merged = early.snapshot();
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(json_object_get(merged,"chats"),"42"),"title")),"live"));
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(json_object_get(merged,"users"),"7"),"first_name")),"live user"));
    assert(!json_object_get(json_object_get(merged,"history"),"42"));
    json_decref(merged);
    early.clear(); early.load();
    merged = early.snapshot();
    assert(json_object_size(json_object_get(merged, "chats")) == 0);
    json_decref(merged); // A delayed load cannot resurrect a cleared account.

    // Startup keeps sorted metadata and separate local small-avatar descriptors.
    UiCache light;
    event(light, R"({"@type":"chat","id":1,"title":"Lower","positions":[{"list":{"@type":"chatListMain"},"order":10}],"photo":{"small":{"@type":"file","id":99}}})");
    event(light, R"({"@type":"chat","id":2,"title":"Top","positions":[{"list":{"@type":"chatListMain"},"order":20}]})");
    event(light, R"({"@type":"updateChatPhoto","chat_id":1,"photo":{"small":{"@type":"file","id":99,"size":4,"local":{"path":"ux0:data/vitagram/files/avatar.png","is_downloading_completed":true}}}})");
    event(light, R"({"@type":"user","id":7,"first_name":"Name","profile_photo":{"small":{"id":99}}})");
    event(light, R"({"@type":"updateNewMessage","message":{"id":55,"chat_id":1,"content":{"@type":"messageText","text":{"text":"History stays separate"}}}})");
    auto* small = light.startupSnapshot();
    assert(!json_object_get(small, "history") && !json_object_get(small, "sticker_items"));
    auto* rows = json_object_get(small, "chats");
    assert(!std::strcmp(json_object_iter_key(json_object_iter(rows)), "2"));
    assert(!json_object_get(json_object_get(rows, "1"), "photo"));
    assert(!json_object_get(json_object_get(json_object_get(small, "users"), "7"), "profile_photo"));
    auto* history = light.history(1); assert(json_object_get(history, "55")); json_decref(history);
    {   // Parser-side persistence of a history reply: same filters, 60 newest, no second copy in remember().
        UiCache parsed;
        std::string text = R"({"@type":"messages","messages":[)";
        for (int id = 1; id <= 70; ++id) text += std::string(id>1?",":"") + "{\"id\":" + std::to_string(id) + ",\"chat_id\":9,\"content\":{\"@type\":\"messageText\"}}";
        text += R"(,{"id":500,"chat_id":9,"sending_state":{"@type":"messageSendingStatePending"}},{"id":501,"chat_id":9,"content":{"is_secret":true}}]})";
        auto* reply = parse(text.c_str());
        parsed.rememberMessages(reply, text.size());
        json_object_set_new(reply, "@ui_remembered", json_true());
        parsed.remember(reply); // Must not store again and must drop the marker.
        assert(!json_object_get(reply, "@ui_remembered")); json_decref(reply);
        auto* kept = parsed.history(9);
        assert(json_object_size(kept) == 60 && json_object_get(kept, "70") && json_object_get(kept, "11") && !json_object_get(kept, "10"));
        assert(!json_object_get(kept, "500") && !json_object_get(kept, "501")); json_decref(kept);
    }
    light.saveStartup(small);
    UiCache next;
    auto* disk_index = next.loadStartup(); assert(disk_index && json_equal(small, disk_index)); json_decref(disk_index);
    auto* avatar=next.chatAvatar(1); // No full-cache load needed.
    assert(avatar && json_integer_value(json_object_get(avatar,"id"))==0);
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(avatar,"local"),"path")),"ux0:data/vitagram/files/avatar.png"));
    json_decref(avatar);
    event(next,R"({"@type":"updateNewChat","chat":{"id":1,"title":"Live without photo","positions":[]}})");
    assert(!next.chatAvatar(1)); // Live removal wins over the old startup index.
    // Interrupted writes retain the previous complete index.
    auto* changed = json_deep_copy(small); json_object_del(json_object_get(changed, "chats"), "2");
    light.saveStartup(changed, [](void*) { return false; }); json_decref(changed);
    disk_index = next.loadStartup(); assert(disk_index && json_equal(small, disk_index)); json_decref(disk_index);
    next.clear(); assert(!next.loadStartup() && !next.chatAvatar(1));
    light.flush(); disk_index = UiCache().loadStartup();
    assert(disk_index && json_equal(small, disk_index)); json_decref(disk_index); json_decref(small);
    UiCache racing;
    load_blocked = true; load_entered = false; load_released = false;
    auto loader = std::async(std::launch::async, [&] { racing.load(); });
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!load_entered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(load_entered);
    event(racing, R"({"@type":"updateNewChat","chat":{"id":1,"title":"Live during read","positions":[]}})");
    event(racing, R"({"@type":"user","id":7,"first_name":"Live during read"})");
    event(racing, R"({"@type":"updateDeleteMessages","chat_id":1,"message_ids":[55]})");
    load_released = true; loader.get(); load_blocked = false;
    small = racing.snapshot();
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(json_object_get(small, "chats"), "1"), "title")), "Live during read"));
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(json_object_get(small, "users"), "7"), "first_name")), "Live during read"));
    assert(!json_object_get(json_object_get(small, "history"), "1")); json_decref(small);
    racing.load(); // Completed loads don't create or replace threads.
    UiCache cancelled_load;
    load_blocked = true; load_entered = false; load_released = false;
    loader = std::async(std::launch::async, [&] { cancelled_load.load(); });
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!load_entered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(load_entered); cancelled_load.clear(); load_released = true;
    loader.get(); load_blocked = false;
    small = cancelled_load.snapshot();
    assert(json_object_size(json_object_get(small, "chats")) == 0); json_decref(small);
    auto* broken_index = json_pack("{s:i,s:{s:{s:i,s:s}},s:{}}", "startup_format", 1, "chats", "1", "id", 1, "title", "Missing positions", "users");
    assert(!json_dump_file(broken_index, VITAGRAM_DB_DIR "/ui-startup.json", JSON_COMPACT));
    UiCache rejects; assert(!rejects.loadStartup());
    json_object_set_new(broken_index, "startup_format", json_integer(2));
    assert(!json_dump_file(broken_index, VITAGRAM_DB_DIR "/ui-startup.json", JSON_COMPACT));
    assert(!rejects.loadStartup()); json_decref(broken_index);
    std::puts("Cache regressions: concurrent live updates, deletion, clear during load and damaged index migration passed");
    UiCache interrupted_read;
    int checkpoints = 0;
    interrupted_read.load([](void* p) { return ++*static_cast<int*>(p) < 3; }, &checkpoints);
    assert(checkpoints == 3 && !interrupted_read.loaded_);
    small = interrupted_read.snapshot();
    assert(json_object_size(json_object_get(small, "chats")) == 0); json_decref(small);
    interrupted_read.load();
    assert(interrupted_read.loaded_);
    small = interrupted_read.snapshot();
    assert(json_object_size(json_object_get(small, "chats")) == 2); json_decref(small);
    std::puts("Cooperative cache: parser cancellation keeps state unpublished and allows a later retry");
    FILE* corrupt = std::fopen(VITAGRAM_DB_DIR "/ui-startup.json", "wb"); assert(corrupt);
    std::fputs("broken", corrupt); std::fclose(corrupt);
    UiCache fallback; assert(!fallback.loadStartup()); fallback.load();
    small = fallback.startupSnapshot(); assert(json_object_size(json_object_get(small, "chats")) == 2); json_decref(small);
    std::puts("Startup index: sorted lightweight snapshot, atomic interruption, clear and legacy fallback passed");

    ExitGesture exit;
    assert(!exit.update(false,true,true,false,100)); // Overlay first.
    assert(!exit.update(true,true,false,false,1000)); // PS+Start while viewing it.
    assert(!exit.update(true,true,false,false,4000000));
    assert(!exit.update(false,false,false,false,5000000));
    assert(!exit.update(true,false,false,false,6000000));
    assert(!exit.update(true,true,true,false,6100000));
    assert(!exit.update(true,true,false,false,8000000));
    assert(exit.update(true,true,false,false,8100000));
    assert(!exit.update(true,true,false,false,9000000));
    assert(!exit.update(true,true,true,false,10000000));
    assert(!exit.update(true,true,false,true,13000000)); // Keyboard cancels arming.
    assert(!exit.update(true,true,false,false,14000000));
    std::puts("Cache: immutable snapshots, concurrent UI, atomic retry/abort, bounded writes; screenshot exit gesture: PASS");
}
