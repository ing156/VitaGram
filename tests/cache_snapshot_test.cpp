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
// The cache is sealed with the console key while the vault is off: never plain JSON on the card.
static json_t* load_cache(const char* path) {
    assert(vault::is_sealed(path) && !vault::is_vault_sealed(path));
    std::string text; if (!vault::read_file(path, 8*1024*1024, text)) return nullptr;
    json_error_t e{}; return json_loadb(text.data(), text.size(), 0, &e);
}
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
    json_t* saved=load_cache(VITAGRAM_DB_DIR "/ui-cache.json"); assert(saved);
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(json_object_get(saved,"chats"),"42"),"title")),"old"));
    json_decref(saved); assert(c.saved_!=c.revision_); c.flush(); assert(c.saved_==c.revision_);
    saved=load_cache(VITAGRAM_DB_DIR "/ui-cache.json"); assert(saved);
    const auto saved_text=dump(saved); json_decref(saved);
    event(c,R"({"@type":"updateChatTitle","chat_id":42,"title":"interrupted"})");
    int calls=0;
    c.flush([](void* p){return ++*static_cast<int*>(p)<3;},&calls);
    assert(c.saved_!=c.revision_);
    saved=load_cache(VITAGRAM_DB_DIR "/ui-cache.json"); assert(saved);
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
    event(light, R"({"@type":"user","id":7,"first_name":"Name","type":{"@type":"userTypeDeleted"},"profile_photo":{"small":{"id":99}}})");
    event(light, R"({"@type":"updateNewMessage","message":{"id":55,"chat_id":1,"content":{"@type":"messageText","text":{"text":"History stays separate"}}}})");
    auto* small = light.startupSnapshot();
    assert(!json_object_get(small, "history") && !json_object_get(small, "sticker_items"));
    auto* rows = json_object_get(small, "chats");
    assert(!std::strcmp(json_object_iter_key(json_object_iter(rows)), "2"));
    assert(!json_object_get(json_object_get(rows, "1"), "photo"));
    assert(!json_object_get(json_object_get(json_object_get(small, "users"), "7"), "profile_photo"));
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(json_object_get(json_object_get(small, "users"), "7"), "type"), "@type")), "userTypeDeleted"));
    auto* full_users = light.snapshot();
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(json_object_get(json_object_get(full_users, "users"), "7"), "type"), "@type")), "userTypeDeleted"));
    json_decref(full_users);
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
        // A reloaded page replaces stored messages; the old trees are freed after the mutex.
        std::string again = R"({"@type":"messages","messages":[)";
        for (int id = 61; id <= 75; ++id) again += std::string(id>61?",":"") + "{\"id\":" + std::to_string(id) + ",\"chat_id\":9,\"content\":{\"@type\":\"messageText\",\"v\":2}}";
        again += "]}";
        reply = parse(again.c_str()); parsed.rememberMessages(reply); json_decref(reply);
        kept = parsed.history(9);
        assert(json_object_size(kept) == 60 && json_object_get(kept, "75") && json_object_get(kept, "16") && !json_object_get(kept, "15"));
        assert(json_integer_value(json_object_get(json_object_get(json_object_get(kept, "65"), "content"), "v")) == 2); json_decref(kept);
        // Histories beyond 20 chats are trimmed from another chat, a few messages per reply.
        for (int chat = 100; chat < 121; ++chat) {
            std::string page = R"({"@type":"messages","messages":[)";
            for (int id = 1; id <= 3; ++id) page += std::string(id>1?",":"") + "{\"id\":" + std::to_string(id) + ",\"chat_id\":" + std::to_string(chat) + "}";
            page += "]}";
            reply = parse(page.c_str()); parsed.rememberMessages(reply); json_decref(reply);
        }
        kept = parsed.history(9); assert(json_object_size(kept) < 60); json_decref(kept);
        kept = parsed.history(120); assert(json_object_size(kept) == 3); json_decref(kept);
    }
    light.saveStartup(small);
    UiCache next;
    auto* disk_index = next.loadStartup(); assert(disk_index && json_equal(small, disk_index)); json_decref(disk_index);
    auto* avatar=next.chatAvatar(1); // No full-cache load needed.
    assert(avatar && json_integer_value(json_object_get(avatar,"id"))==0);
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(avatar,"local"),"path")),"ux0:data/vitagram/files/avatar.png"));
    json_decref(avatar);
    event(next,R"({"@type":"updateNewChat","chat":{"id":1,"title":"Live, photo not downloaded","positions":[],"photo":{"small":{"@type":"file","id":5,"local":{"is_downloading_completed":false}}}}})");
    avatar=next.chatAvatar(1); assert(avatar && json_object_get(avatar,"local")); json_decref(avatar); // Index picture until TDLib downloads.
    event(next,R"({"@type":"updateNewChat","chat":{"id":1,"title":"Live without photo","positions":[]}})");
    assert(!next.chatAvatar(1)); // Live removal wins over the old startup index.
    // Interrupted writes retain the previous complete index.
    auto* changed = json_deep_copy(small); json_object_del(json_object_get(changed, "chats"), "2");
    light.saveStartup(changed, [](void*) { return false; }); json_decref(changed);
    disk_index = next.loadStartup(); assert(disk_index && json_equal(small, disk_index)); json_decref(disk_index);
    next.clear(); assert(!next.loadStartup() && !next.chatAvatar(1));
    light.flush(); disk_index = UiCache().loadStartup();
    assert(disk_index && json_equal(small, disk_index)); json_decref(disk_index); json_decref(small);
    {   // The confirmed list borrows cached photos without copying the live cache under its mutex.
        auto* pending = parse(R"({"1":{"@type":"chat","id":1,"title":"Lower","positions":[{"list":{"@type":"chatListMain"},"order":10}]},)"
                              R"("2":{"@type":"chat","id":2,"title":"Top","positions":[{"list":{"@type":"chatListMain"},"order":20}],"@avatar_path":"ux0:data/vitagram/files/top.png"}})");
        assert(!light.saveStartupList(pending, light.generation())); // Not merged with disk yet.
        light.load();
        assert(light.saveStartupList(pending, light.generation()));
        json_decref(pending);
        auto* list = UiCache().loadStartup(); assert(list);
        assert(!std::strcmp(json_object_iter_key(json_object_iter(json_object_get(list, "chats"))), "2"));
        auto* avatar = json_object_get(json_object_get(list, "avatars"), "1");
        assert(!std::strcmp(json_string_value(json_object_get(json_object_get(avatar, "local"), "path")), "ux0:data/vitagram/files/avatar.png"));
        // A picture downloaded this session (shown, but unknown to the cached photo) is kept too.
        avatar = json_object_get(json_object_get(list, "avatars"), "2");
        assert(avatar && !std::strcmp(json_string_value(json_object_get(json_object_get(avatar, "local"), "path")), "ux0:data/vitagram/files/top.png"));
        assert(!json_object_get(json_object_get(json_object_get(list, "chats"), "2"), "@avatar_path"));
        json_decref(list);
        auto* live = light.snapshot(); // The cached photo itself is untouched.
        assert(json_integer_value(json_object_get(json_object_get(json_object_get(json_object_get(json_object_get(live, "chats"), "1"), "photo"), "small"), "size")) == 4);
        json_decref(live);
    }
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

    {   // Histories: one file per chat, merged lazily; the legacy monolithic history migrates.
        auto* legacy = parse(R"({"chats":{"42":{"id":42,"title":"Legacy","positions":[]}},"history":{"42":{)"
                             R"("1":{"id":1,"chat_id":42},"2":{"id":2,"chat_id":42},"3":{"id":3,"chat_id":42}}}})");
        assert(!json_dump_file(legacy, VITAGRAM_DB_DIR "/ui-cache.json", JSON_COMPACT)); json_decref(legacy);
        UiCache::removeHistoryFiles();
        {
            UiCache migrating; migrating.load();
            auto* kept = migrating.history(42); assert(json_object_size(kept) == 3); json_decref(kept);
            kept = migrating.history(42, 2); // Only the newest are copied.
            assert(json_object_size(kept) == 2 && json_object_get(kept, "3") && !json_object_get(kept, "1")); json_decref(kept);
            migrating.flush(); assert(migrating.saved_ == migrating.revision_ && migrating.history_dirty_.empty());
        }
        auto* main_file = load_cache(VITAGRAM_DB_DIR "/ui-cache.json"); assert(main_file);
        assert(!json_object_get(main_file, "history") && json_object_get(json_object_get(main_file, "chats"), "42"));
        json_decref(main_file);
        auto* chat_file = load_cache(VITAGRAM_DB_DIR "/ui-history/42.json");
        assert(chat_file && json_object_size(chat_file) == 3); json_decref(chat_file);

        UiCache lazy; lazy.load();
        assert(lazy.historyPending(42) && !lazy.history(42)); // Startup reads no history file.
        event(lazy, R"({"@type":"updateDeleteMessages","chat_id":42,"message_ids":[2]})");
        event(lazy, R"({"@type":"updateNewMessage","message":{"id":9,"chat_id":42,"content":{"@type":"messageText","v":"live"}}})");
        event(lazy, R"({"@type":"updateChatTitle","chat_id":42,"title":"Renamed"})");
        lazy.flush(); // The unmerged chat file is kept, not overwritten by the one live message.
        chat_file = load_cache(VITAGRAM_DB_DIR "/ui-history/42.json");
        assert(chat_file && json_object_size(chat_file) == 3); json_decref(chat_file);
        assert(lazy.loadHistory(99) && !lazy.historyPending(42) && !lazy.loadHistory());
        auto* kept = lazy.history(42);
        assert(json_object_size(kept) == 3 && json_object_get(kept, "1") && !json_object_get(kept, "2") && json_object_get(kept, "9"));
        json_decref(kept);
        assert(lazy.saved_ != lazy.revision_); lazy.flush(); assert(lazy.saved_ == lazy.revision_);
        chat_file = load_cache(VITAGRAM_DB_DIR "/ui-history/42.json");
        assert(chat_file && json_object_size(chat_file) == 3 && !json_object_get(chat_file, "2")); json_decref(chat_file);
        // Only the changed chat is written once the main file is current.
        const auto main_saved = lazy.main_saved_;
        event(lazy, R"({"@type":"updateMessageEdited","chat_id":42,"message_id":9,"edit_date":5})");
        assert(lazy.main_revision_ == main_saved); lazy.flush();
        chat_file = load_cache(VITAGRAM_DB_DIR "/ui-history/42.json");
        assert(json_integer_value(json_object_get(json_object_get(chat_file, "9"), "edit_date")) == 5); json_decref(chat_file);
        // An interrupted chat write stays dirty for the next flush.
        event(lazy, R"({"@type":"updateMessageEdited","chat_id":42,"message_id":9,"edit_date":6})");
        lazy.flush([](void*) { return false; });
        lazy.flush([](void* p) { return ++*static_cast<int*>(p) < 2; }, &checkpoints);
        assert(lazy.history_dirty_.count(42) && lazy.saved_ != lazy.revision_);
        lazy.flush(); assert(lazy.history_dirty_.empty());
        // Another account's histories are never merged: clear() removes every file on the next flush.
        lazy.clear(); lazy.flush();
        struct stat gone{}; assert(stat(VITAGRAM_DB_DIR "/ui-history/42.json", &gone));
        UiCache after_clear; after_clear.load(); assert(!after_clear.historyPending(42) && !after_clear.loadHistory());
    }
    std::puts("Per-chat histories: legacy migration, lazy merge with live deletions, partial writes, wipe passed");

    ExitGesture exit;
    assert(!exit.update(false,true,true,false,100)); // Select first.
    assert(!exit.update(true,true,false,false,1000)); // Start while Select is held must not arm exit.
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
