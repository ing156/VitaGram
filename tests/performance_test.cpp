#include "td_dispatch_policy.h"
#include "td_unused_update.h"
#include "performance_log.h"
#include "td_event_queue.h"
#include "td/utils/VitaTrace.h"
#include <cassert>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace performance;
int main(int argc, char** argv) {
    assert(argc == 2);
    {
        Collector slow_collector;
        SlowEvent event;event.phase=SlowPhase::Process;
        std::strcpy(event.type.data(),"updateNewMessage");event.wire_bytes=1234;
        event.elapsed=49999;slow_collector.slow(event);
        assert(slow_collector.take(1).slow_count==0);
        for (int i=0;i<75;++i) { event.elapsed=50000+i;slow_collector.slow(event); }
        auto snapshot=slow_collector.take(2);
        assert(snapshot.slow_count==(enabled ? slow_limit : 0));
        assert(snapshot.slow_dropped==(enabled ? 43u : 0u));
        for (size_t i=0;i<snapshot.slow_count;++i) {
            assert(snapshot.slow[i].elapsed>=50043);
            assert(snapshot.slow[i].wire_bytes==1234);
        }
        assert(slow_collector.take(3).slow_count==0);
        FILE* file=std::tmpfile();assert(file);writeSnapshot(file,snapshot,"test");std::rewind(file);
        std::string written;char line[512];while (std::fgets(line,sizeof(line),file)) written+=line;std::fclose(file);
        assert((written.find("slow_event phase=process type=updateNewMessage")!=std::string::npos)==enabled);
        assert((written.find("slow_events threshold_us=50000 kept=32 omitted=43")!=std::string::npos)==enabled);
    }
    collector().take(now());
    {
        std::string temporary="updateUser\ninjected";
        SlowScope slow(SlowPhase::Ui,temporary.c_str(),99);
        temporary.assign(100,'x');
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    auto scoped=collector().take(now());
    assert(scoped.slow_count==(enabled ? 1u : 0u));
    if (enabled) {
        assert(std::string(scoped.slow[0].type.data())=="updateUser_injected");
        assert(scoped.slow[0].wire_bytes==99);
        assert(scoped.slow[0].phase==SlowPhase::Ui);
    }
    Collector c;
    c.start(100);
    {
        td::VitaTrace actor(td::VitaPhase::Actor);
        const auto outer = td_diagnostics::active().load();
        assert(!enabled || outer >> 56 == 1);
        {
            td::VitaTrace socket(td::VitaPhase::SocketRead);
            assert(!enabled || td_diagnostics::active().load() >> 56 == 3);
            FILE* live = std::tmpfile(); assert(live);
            td_diagnostics::write(live); std::rewind(live);
            char line[128]{}; assert(std::fgets(line, sizeof(line), live)); std::fclose(live);
            assert(std::string(line).find(enabled ? "phase=socket_read" : "phase=idle") != std::string::npos);
        }
        assert(td_diagnostics::active().load() == outer);
        for (auto phase : {td::VitaPhase::FileIo, td::VitaPhase::LogWrite, td::VitaPhase::SocketConnect, td::VitaPhase::FileSync, td::VitaPhase::FileMeta}) {
            td::VitaTrace operation(phase);
            assert(!enabled || td_diagnostics::active().load() >> 56 == static_cast<unsigned>(phase));
        }
        assert(td_diagnostics::active().load() == outer);
        td_diagnostics::path("ux0:data/vitagram/files/profile_photos/12345.jpg");
        assert(!enabled || std::string(td_diagnostics::path_label().data()) == "files_profile_photos");
        td_diagnostics::path("ux0:data/vitagram/db/td.binlog");
        assert(!enabled || std::string(td_diagnostics::path_label().data()) == "vitagram_db");
        td_diagnostics::path_label()[0] = 0;
    }
    assert(!td_diagnostics::active().load());
    auto traced = collector().take(now());
    assert(traced.get(Metric::TdActor).count == (enabled ? 1u : 0u));
    assert(traced.get(Metric::TdSocketRead).count == (enabled ? 1u : 0u));
    for (auto metric : {Metric::TdFileIo, Metric::TdLogWrite, Metric::TdSocketConnect, Metric::TdFileSync, Metric::TdFileMeta})
        assert(traced.get(metric).count == (enabled ? 1u : 0u));
    if (!enabled) {
        c.observe(Metric::Frame, 1000); c.queues(1, 2, 3, 4, 5);
        auto s = c.take(200);
        assert(!s.get(Metric::Frame).count && !s.gauges[0].current);
        assert(flush("/missing/perf", "/missing/previous", "test", 300));
        return 0;
    }
    Histogram h;
    assert(!h.percentile(95));
    for (int i = 0; i < 95; ++i) h.add(1000);
    for (int i = 0; i < 4; ++i) h.add(17000);
    h.add(120000000);
    assert(h.count == 100 && h.percentile(50) == 1000 && h.percentile(95) == 1000);
    assert(h.percentile(99) == 20000 && h.percentile(100) == 120000000);
    c.observe(Metric::Frame, -1);
    c.observe(Metric::Frame, 0);
    c.observe(Metric::Frame, 21000);
    c.queues(2, 400, 3, 900, 1);
    c.queues(1, 200, 0, 0, 0);
    auto s = c.take(300);
    assert(s.from == 100 && s.until == 300 && s.get(Metric::Frame).count == 2);
    assert(s.get(Metric::Frame).maximum == 21000 && s.gauges[1].peak == 400);
    s = c.take(400);
    assert(s.from == 300 && !s.get(Metric::Frame).count);
    assert(s.gauges[1].current == 200 && s.gauges[1].peak == 200);

    // Under contention a sample is either accounted for or explicitly dropped.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&] {
        for (int n = 0; n < 20000; ++n) c.observe(Metric::Process, 1500);
    });
    for (auto& thread : threads) thread.join();
    s = c.take(500);
    assert(s.get(Metric::Process).count + s.dropped == 80000);
    assert(s.get(Metric::Process).sum == s.get(Metric::Process).count * 1500);

    // Coalescing removes precisely the old wire payload, preserving controls.
    TdEventQueue queue;
    auto event = [](int bytes) {
        return json_pack("{s:s,s:i,s:{s:i}}", "@type", "updateFile", "@wire_bytes", bytes, "file", "id", 10);
    };
    assert(enqueue_td_event(queue, event(100)) == 0);
    auto* control = json_pack("{s:s}", "@type", "updateMessageSendSucceeded");
    assert(enqueue_td_event(queue, control) == 0);
    assert(enqueue_td_event(queue, event(200)) == 100 && queue.size() == 2);
    auto* first = static_cast<json_t*>(take_td_event(queue, true, false));
    assert(first == control && !td_event_wire_bytes(first)); json_decref(first);
    auto* last = static_cast<json_t*>(take_td_event(queue, false, false));
    assert(td_event_wire_bytes(last) == 200); json_decref(last);

    auto* background=json_pack("{s:s,s:I}","@type","updateChatTitle","chat_id",(json_int_t)7);
    auto* active=json_pack("{s:s,s:I}","@type","updateDeleteMessages","chat_id",(json_int_t)42);
    auto* history=json_pack("{s:s,s:{s:s,s:I,s:i}}","@type","messages","@extra","kind","history","chat",(json_int_t)42,"generation",3);
    auto* stale=json_pack("{s:s,s:{s:s,s:I,s:i}}","@type","messages","@extra","kind","history","chat",(json_int_t)42,"generation",2);
    for (auto* root:{background,stale,active,history}) queue.push_back(root);
    assert(!take_td_event(queue,true,false,42,3));
    assert(take_td_event(queue,false,false,42,3)==active);json_decref(active);
    assert(take_td_event(queue,false,true,42,3)==background);json_decref(background);
    assert(take_td_event(queue,false,false,42,3)==history);json_decref(history);
    assert(take_td_event(queue,false,false,99,3)==stale);json_decref(stale);

    // A large unrelated backlog must not affect which active event is selected.
    for (int i=0;i<10000;++i) queue.push_back(json_pack("{s:s,s:i}","@type","updateOption","sequence",i));
    auto* current42=json_pack("{s:s,s:I}","@type","updateMessageEdited","chat_id",(json_int_t)42);
    auto* current99=json_pack("{s:s,s:I}","@type","updateMessageInteractionInfo","chat_id",(json_int_t)99);
    auto* old_history=json_pack("{s:s,s:{s:s,s:I,s:i}}","@type","messages","@extra","kind","history","chat",(json_int_t)42,"generation",2);
    auto* new_history=json_pack("{s:s,s:{s:s,s:I,s:i}}","@type","messages","@extra","kind","history","chat",(json_int_t)42,"generation",3);
    for (auto* root:{current42,current99,old_history,new_history}) queue.push_back(root);
    assert(take_td_event(queue,false,false,42,3)==current42);json_decref(current42);
    assert(take_td_event(queue,false,false,99,3)==current99);json_decref(current99);
    assert(take_td_event(queue,false,false,42,3)==new_history);json_decref(new_history);
    assert(take_td_event(queue,false,false,42,2)==old_history);json_decref(old_history);
    for (int i=0;i<10000;++i) {
        auto* root=static_cast<json_t*>(take_td_event(queue,false,false,12345,0));
        assert(json_integer_value(json_object_get(root,"sequence"))==i);json_decref(root);
    }
    assert(queue.empty());
    // Removing a bucket head or replacing a file must leave the other indices valid.
    auto* old_file=event(100);enqueue_td_event(queue,old_file);
    auto* other=json_pack("{s:s,s:I}","@type","updateMessageEdited","chat_id",(json_int_t)42);queue.push_back(other);
    assert(enqueue_td_event(queue,event(200))==100);
    assert(take_td_event(queue,false,false,42,0)==other);json_decref(other);
    auto* file=static_cast<json_t*>(queue.front());assert(td_event_wire_bytes(file)==200);
    queue.pop_front();json_decref(file);assert(queue.empty());

    auto* compact=compact_raw_unused_td_update(R"({"@type":"updateStickerSet","sticker_set":{"stickers":[{"emoji":"X","nested":{"@extra":"body only","escaped":"\"}"}}]}})");
    assert(compact && json_object_size(compact)==1);json_decref(compact);
    for (const char* raw:{R"({"@type":"updateStickerSet","@extra":null})",R"({"@type":"updateStickerSet","@type":"message"})",R"({"@type":"updateStickerSet","value":[1,]})",R"({"@type":"updateStickerSet","value":"\uD800"})",R"({"@type":"messages","value":"updateStickerSet"})",R"({"@type":"updateStickerSet"} garbage)"})
        assert(!compact_raw_unused_td_update(raw));
    compact=compact_raw_unused_td_update(R"({"@type":"updateStickerSet","value":[null,true,false,-1.5e+2,"\uD83D\uDE00"]})");
    assert(compact);json_decref(compact);

    assert(td_pump_budget_us(4000,true,false)==500);
    assert(td_pump_budget_us(4000,false,true)==2000);
    assert(td_pump_budget_us(127,false,false)==2000);
    assert(td_pump_budget_us(128,false,false)==4000);
    assert(td_pump_budget_us(512,false,false)==6000);
    assert(td_pump_budget_us(4000,false,false)==8000);
    assert(td_request_allowed(2,true,true,false));
    assert(td_request_allowed(1,true,true,false));
    assert(!td_request_allowed(1,false,true,false));
    assert(!td_request_allowed(1,true,true,true));
    // The open chat waits for history: background downloads stay queued.
    assert(!td_request_allowed(1,true,false,false,true));
    assert(td_request_allowed(9,false,false,false,true));
    assert(td_request_allowed(5,false,false,true));
    auto* classified=json_pack("{s:s,s:i,s:{s:s,s:I,s:i},s:i}","@type","messages","@wire_bytes",123,"@extra","kind","history","chat",(json_int_t)42,"generation",7,"@received_at",1000);
    queue.push_back(classified);TdEventMetadata selected;
    assert(take_td_event(queue,false,false,42,7,&selected)==classified);
    assert(selected.chat==42 && selected.response==TdResponseKind::History && selected.bytes==123 && selected.received==1000);
    json_decref(classified);

    std::string path = std::string(argv[1]) + "/performance.log", previous = path + ".previous";
    FILE* f = std::fopen(path.c_str(), "wb"); assert(f);
    assert(!std::fseek(f, 1024*1024, SEEK_SET)); std::fputc('x', f); std::fclose(f);
    collector().start(1000); observe(Metric::Frame, 18000);
    assert(flush(path.c_str(), previous.c_str(), "test", 2000));
    assert(!access(previous.c_str(), F_OK));
    f = std::fopen(path.c_str(), "rb"); assert(f);
    char buffer[8192]{}; auto bytes = std::fread(buffer, 1, sizeof(buffer)-1, f); std::fclose(f);
    assert(bytes && std::string(buffer).find("version=test") != std::string::npos);
    assert(std::string(buffer).find("frame_us n=1") != std::string::npos);
    std::string display_text; readDisplay(display_text);
    assert(display_text.find("Frame p95<=18.0") != std::string::npos);
    assert(!flush((path+"/missing").c_str(), previous.c_str(), "test", 3000));
    readDisplay(display_text); assert(display_text.find("write failed") != std::string::npos);
    assert(!collector().take(4000).get(Metric::Frame).count);
    std::puts("Performance tests passed");
}
