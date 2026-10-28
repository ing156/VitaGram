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
        auto stream = std::make_shared<download_stream::State>(7);
        stream->update("private-media-name.mp3", 1000000, 0, 262144, false);
        auto written = [] {
            FILE* file = std::tmpfile(); assert(file);
            audio_startup_trace::write(file); std::rewind(file);
            std::string text; char line[512];
            while (std::fgets(line, sizeof(line), file)) text += line;
            std::fclose(file); return text;
        };
        diag_log::set_mask(0);
        audio_startup_trace::record(stream.get(), 3, "disabled");
        assert(written().empty());
        diag_log::set_mask(1u << static_cast<unsigned>(diag_log::Kind::Performance));
        const int64_t origin = now();
        audio_startup_trace::record(stream.get(), 3, "open_input_begin", 0, origin, stream, 7);
        stream->update("private-final-name.mp3", 1000000, 0, 1000000, true);
        audio_startup_trace::record(stream.get(), 3, "open_input_end", -5, origin, stream, 7);
        if (enabled) {
            assert(audio_startup_trace::events[1].at >= audio_startup_trace::events[0].at);
            assert(audio_startup_trace::events[1].total == audio_startup_trace::events[1].at - origin);
        }
        const auto text = written();
        if (enabled) {
            assert(text.find("generation=3 stage=open_input_begin at_us=") != std::string::npos);
            assert(text.find("result=0 file_id=7 size=1000000 prefix=262144 complete=0") != std::string::npos);
            assert(text.find("result=-5 file_id=7 size=1000000 prefix=1000000 complete=1") != std::string::npos);
            assert(text.find("private-") == std::string::npos);
        } else assert(text.empty());
        assert(written().empty());
        for (size_t i = 0; i < audio_startup_trace::capacity + 1; ++i)
            audio_startup_trace::record(stream.get(), 4, "bounded");
        const auto full = written();
        assert((full.find("audio_startup_events kept=128 dropped=1") != std::string::npos) == enabled);
        assert(written().empty());
        // A busy writer must never stall the playback or UI thread.
        pthread_mutex_lock(&audio_startup_trace::mutex);
        audio_startup_trace::record(stream.get(), 4, "contended");
        pthread_mutex_unlock(&audio_startup_trace::mutex);
        assert((written().find("kept=0 dropped=1") != std::string::npos) == enabled);
        diag_log::set_mask(0);
    }
    {
        int job = 0;
        video_diagnostics::update(&job, "pace_packet", 17, 8, 12345, 6789);
        video_diagnostics::Playback playback{5000000, 5100000, 7000, 8000, false, true, true, false, true, true};
        const auto entered = video_diagnostics::entries[0].at;
        video_diagnostics::update(&job, "pace_packet", 17, 8, 23456, 7890, playback);
        if (enabled) assert(video_diagnostics::entries[0].at == entered);
        {
            std::lock_guard<std::mutex> lock(video_diagnostics::mutex);
            std::thread writer([&] { video_diagnostics::update(&job, "blocked", 0, 0, 0, 0); });
            writer.join();
        }
        FILE* file = std::tmpfile(); assert(file);
        video_diagnostics::write(file); std::rewind(file);
        char line[1024]{}; const bool found = std::fgets(line, sizeof(line), file) != nullptr; std::fclose(file);
        assert(found == enabled);
        if (enabled) assert(std::string(line).find("stage=pace_packet") != std::string::npos &&
                            std::string(line).find("packets=17 frames=8 audio_clock_at_stage_us=23456 video_position_at_stage_us=7890") != std::string::npos);
        if (enabled) {
            assert(std::string(line).find("packet_dts_us=5000000 packet_pts_us=5100000 frame_pts_us=7000 due_us=8000") != std::string::npos);
            assert(std::string(line).find("paused=0 audio_playing=1 clock_ready=1 audio_buffering=0 frame_stalled=1 frame_lagging=1") != std::string::npos);
        }
        video_diagnostics::remove(&job);
    }
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
    {
        Collector retention;
        SlowEvent aggregate; aggregate.phase = SlowPhase::Ui;
        std::strcpy(aggregate.type.data(), "frame_ui"); aggregate.elapsed = 1000000;
        for (size_t i = 0; i < slow_limit; ++i) retention.slow(aggregate);
        SlowEvent cause; cause.phase = SlowPhase::Ui;
        std::strcpy(cause.type.data(), "web_paint"); cause.elapsed = 60000;
        cause.context = {42, Screen::WebApp, true, true, true, true, 1, 544, 960};
        retention.slow(cause);
        auto snapshot = retention.take(1);
        if (enabled) {
            bool found = false;
            for (size_t i = 0; i < snapshot.slow_count; ++i) {
                if (!std::strcmp(snapshot.slow[i].type.data(), "web_paint")) found = true;
            }
            assert(found);
            FILE* file = std::tmpfile(); assert(file);
            writeSnapshot(file, snapshot, "test"); std::rewind(file);
            std::string written; char line[1024];
            while (std::fgets(line, sizeof(line), file)) written += line;
            std::fclose(file);
            assert(written.find("frame=42 screen=webapp combined=1 keyboard=1 scrolling=1 fullscreen=1 rotation=1 web_width=544 web_height=960") != std::string::npos);
            assert(written.find("dropped_samples_kind=diagnostic_measurements") != std::string::npos);
        }
        retention.count(Counter::Received, 17);
        retention.count(Counter::Processed, 8);
        for (int duration : {16000, 33334, 40000, 50000, 100000, 110000})
            retention.observe(Metric::Frame, duration);
        snapshot = retention.take(2);
        assert(snapshot.counters[static_cast<size_t>(Counter::Received)] == (enabled ? 17u : 0u));
        assert(snapshot.counters[static_cast<size_t>(Counter::Processed)] == (enabled ? 8u : 0u));
        assert(snapshot.counters[static_cast<size_t>(Counter::FrameOver33)] == (enabled ? 4u : 0u));
        assert(snapshot.counters[static_cast<size_t>(Counter::FrameOver50)] == (enabled ? 2u : 0u));
        assert(snapshot.counters[static_cast<size_t>(Counter::FrameOver100)] == (enabled ? 1u : 0u));
        snapshot = retention.take(3);
        for (auto count : snapshot.counters) assert(!count);
        {
            ui_diagnostics::Stage stage(ui_diagnostics::Phase::WebPaint);
            assert(!enabled || ui_diagnostics::active().load() >> 56 == static_cast<unsigned>(ui_diagnostics::Phase::WebPaint));
            assert(std::string(ui_diagnostics::name(static_cast<unsigned>(ui_diagnostics::Phase::WebPaint))) == "web_paint");
        }
        // Other threads cannot inherit UI frame/screen attribution.
        frame_context = {42, Screen::WebApp};
        std::thread background([] { assert(!frame_context.frame && frame_context.screen == Screen::Unknown); });
        background.join();
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
        assert(scoped.slow[0].context.frame==42 && scoped.slow[0].context.screen==Screen::WebApp);
    }
    {
        SlowScope steps(SlowPhase::Ui, "post_scheduler");
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        steps.next("post_textures");
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        steps.finish();
    }
    auto steps = collector().take(now());
    assert(steps.slow_count == (enabled ? 2u : 0u));
    if (enabled) {
        assert(std::string(steps.slow[0].type.data()) == "post_scheduler");
        assert(std::string(steps.slow[1].type.data()) == "post_textures");
        assert(steps.slow[1].elapsed < 110000); // The second step starts its own timer.
    }
    {
        td::VitaTrace actor(td::VitaPhase::Actor);
        {
            td::VitaTrace write(td::VitaPhase::FilePwrite);
            td::vita_trace_file("ux0:data/vitagram/files/temp/private-name.mp4", 17, 1048576, 65536);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        {
            td::VitaTrace write(td::VitaPhase::FileWrite);
            td::vita_trace_file("ur0:data/vitagram/db/secret.binlog", 23, -1, 4096);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        {
            td::VitaTrace metadata(td::VitaPhase::FileMeta);
            td::vita_trace_path("ux0:data/vitagram/files/photos/private.jpg");
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
    }
    auto files = collector().take(now());
    assert(files.slow_count == (enabled ? 3u : 0u));
    if (enabled) {
        assert(std::string(files.slow[0].file_dir.data()) == "ux0:files_temp");
        assert(files.slow[0].file_fd == 17 && files.slow[0].file_offset == 1048576 && files.slow[0].file_bytes == 65536);
        assert(std::string(files.slow[1].file_dir.data()) == "ur0:vitagram_db");
        assert(files.slow[1].file_fd == 23 && files.slow[1].file_offset == -1 && files.slow[1].file_bytes == 4096);
        assert(files.slow[2].file_fd == -1); // Metadata must not inherit the previous descriptor.
        FILE* report = std::tmpfile(); assert(report);
        writeSnapshot(report, files, "test"); std::rewind(report);
        std::string output; char line[1024];
        while (std::fgets(line, sizeof(line), report)) output += line;
        std::fclose(report);
        assert(output.find("file_dir=ux0:files_temp file_fd=17 file_offset=1048576 file_bytes=65536") != std::string::npos);
        assert(output.find("private-name") == std::string::npos && output.find("secret.binlog") == std::string::npos);
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
        for (auto phase : {td::VitaPhase::FileRead, td::VitaPhase::FileWrite, td::VitaPhase::FilePread, td::VitaPhase::FilePwrite, td::VitaPhase::FileIo, td::VitaPhase::LogWrite, td::VitaPhase::SocketConnect, td::VitaPhase::FileSync, td::VitaPhase::FileMeta}) {
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
        assert(traced.get(metric).count == (enabled ? (metric == Metric::TdFileIo ? 5u : 1u) : 0u));
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

    {
        TdEventQueue ages;
        assert(!ages.oldestParsedAt());
        auto* normal = json_pack("{s:s}", "@type", "updateChatTitle");
        auto* urgent = json_pack("{s:s}", "@type", "updateMessageSendSucceeded");
        TdEventMetadata n; n.parsed = 100;
        TdEventMetadata u; u.parsed = 150; u.urgent = true;
        ages.enqueue(normal, n); ages.enqueue(urgent, u);
        assert(ages.oldestParsedAt() == 100);
        auto* selected = static_cast<json_t*>(ages.take(false, true));
        assert(selected == normal); json_decref(selected);
        assert(ages.oldestParsedAt() == 150);
        selected = static_cast<json_t*>(ages.take(false, false));
        assert(selected == urgent); json_decref(selected);
        assert(!ages.oldestParsedAt());
        // Replacing a coalesced file must remove its old timestamp as well.
        for (int timestamp : {200, 300}) {
            auto* file = json_pack("{s:s}", "@type", "updateFile");
            TdEventMetadata m; m.file = 10; m.parsed = timestamp;
            ages.enqueue(file, m);
        }
        assert(ages.size() == 1 && ages.oldestParsedAt() == 300);
        selected = static_cast<json_t*>(ages.take(false, false)); json_decref(selected);
    }
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
    for (const char* type:{"updateAvailableMessageEffects","updateEmojiChatThemes","updateTextCompositionStyles","updateAttachmentMenuBots","updateProfileAccentColors"}) {
        std::string raw=std::string("{\"@type\":\"")+type+"\",\"themes\":[{\"name\":\"x\\u00e9\",\"n\":[1,2.5e3,null]}]}";
        auto* stub=compact_raw_unused_td_update(raw);
        assert(stub && std::string(json_string_value(json_object_get(stub,"@type")))==type && json_object_size(stub)==1);
        json_decref(stub);
        assert(!compact_raw_unused_td_update(std::string("{\"@type\":\"")+type+"\",\"@extra\":\"x\"}"));
    }
    assert(!compact_raw_unused_td_update(R"({"@type":"updateAccentColors","colors":[{"id":8}]})"));
    assert(!compact_raw_unused_td_update(R"({"@type":"updateUser","user":{"id":1,"note":"updateEmojiChatThemes"}})"));
    compact=compact_raw_unused_td_update(R"({"@type":"updateStickerSet","value":[null,true,false,-1.5e+2,"\uD83D\uDE00"]})");
    assert(compact);json_decref(compact);

    assert(td_pump_budget_us(4000,true,false)==500);
    assert(td_pump_budget_us(4000,false,true)==2000);
    assert(td_pump_budget_us(127,false,false)==2000);
    assert(td_pump_budget_us(128,false,false)==4000);
    assert(td_pump_budget_us(512,false,false)==6000);
    assert(td_pump_budget_us(4000,false,false)==8000);
    // Startup flood before the first live chat list: a larger budget, but input still wins.
    assert(td_pump_budget_us(32,false,false,true)==kTdStartupPumpUs);
    assert(td_pump_budget_us(4000,false,false,true)==kTdStartupPumpUs);
    assert(td_pump_budget_us(31,false,false,true)==2000);
    assert(td_pump_budget_us(4000,true,false,true)==500);
    assert(td_pump_budget_us(4000,false,true,true)==2000);
    assert(td_request_allowed(2,true,true,false));
    assert(td_request_allowed(1,true,true,false));
    assert(!td_request_allowed(1,false,true,false));
    assert(!td_request_allowed(1,true,true,true));
    // The open chat waits for history: background downloads stay queued.
    assert(!td_request_allowed(1,true,false,false,true));
    assert(td_request_allowed(9,false,false,false,true));
    // ...but not indefinitely: an open video must not starve thumbnails and avatars.
    assert(!td_request_allowed(1,false,false,false,true,kTdHoldMaxUs-1));
    assert(td_request_allowed(1,false,false,false,true,kTdHoldMaxUs));
    assert(td_request_allowed(1,false,false,true,false,kTdHoldMaxUs));
    assert(!td_request_allowed(1,false,true,false,true,kTdHoldMaxUs)); // Congestion still applies.
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
    diag_log::set_mask(1u << static_cast<unsigned>(diag_log::Kind::Performance));
    audio_startup_trace::record(nullptr, 17, "flush_integration");
    assert(flush(path.c_str(), previous.c_str(), "test", 2000));
    diag_log::set_mask(0);
    assert(!access(previous.c_str(), F_OK));
    f = std::fopen(path.c_str(), "rb"); assert(f);
    char buffer[8192]{}; auto bytes = std::fread(buffer, 1, sizeof(buffer)-1, f); std::fclose(f);
    assert(bytes && std::string(buffer).find("version=test") != std::string::npos);
    assert(std::string(buffer).find("frame_us n=1") != std::string::npos);
    assert(std::string(buffer).find("generation=17 stage=flush_integration") != std::string::npos);
    assert(!flush((path+"/missing").c_str(), previous.c_str(), "test", 3000));
    assert(!collector().take(4000).get(Metric::Frame).count);
    // Over budget only replaceable values are shed (F07); persistent state is never classified so.
    assert(td_event_is_ephemeral(R"({"@type":"updateUserStatus","user_id":1,"status":{}})"));
    assert(td_event_is_ephemeral(R"({"@type":"updateChatAction","chat_id":1})"));
    assert(td_event_is_ephemeral(R"({"@type":"updateChatOnlineMemberCount","chat_id":1,"online_member_count":3})"));
    assert(!td_event_is_ephemeral(R"({"@type":"updateChatActionBar","chat_id":1})"));
    assert(!td_event_is_ephemeral(R"({"@type":"updateNewMessage","message":{}})"));
    assert(!td_event_is_ephemeral(R"({"@type":"updateFile","file":{}})"));
    assert(!td_event_is_ephemeral(R"({"@type":"updateUserStatusX"})") && !td_event_is_ephemeral(""));
    // Hard limit with hysteresis: pause above it, resume only under the high-water mark (F07).
    assert(!td_queue_paused(false, kTdQueueHardBytes, kTdQueueHardEvents));
    assert(td_queue_paused(false, kTdQueueHardBytes + 1, 0) && td_queue_paused(false, 0, kTdQueueHardEvents + 1));
    assert(td_queue_paused(true, kTdQueueHighWaterBytes + 1, 0) && td_queue_paused(true, 0, kTdQueueHighWaterEvents + 1));
    assert(!td_queue_paused(true, kTdQueueHighWaterBytes, kTdQueueHighWaterEvents));
    std::puts("Performance tests passed");
}
