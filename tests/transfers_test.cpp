#include <algorithm>
#include <atomic>
#include <deque>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <chrono>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <jansson.h>
#define private public
#include "telegram_client.h"
#undef private
#include "td_event_queue.h"
#include "cpu_scheduler.h"
#include <psp2/io/stat.h>
#include <td/telegram/td_json_client.h>

int64_t sceKernelGetSystemTimeWide() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int sceKernelDelayThread(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); return 0; }
int sceIoGetstat(const char* p, SceIoStat* out) { struct stat s{}; int rc = stat(p, &s); out->st_size=s.st_size; return rc; }
int sceIoMkdir(const char* p, int m) { return mkdir(p, m); }
namespace cpu_scheduler {
Thread::Thread(Role):id_(0) {} Thread::~Thread() {}
Media::Media(Role r):role_(r) {} Media::~Media() {}
}
static std::atomic<int> receive_count{0}, produced{0}, sent{0};
static std::thread::id td_thread;
extern "C" {
void* td_json_client_create() { td_thread=std::this_thread::get_id(); return &sent; }
void td_json_client_destroy(void*) { assert(td_thread==std::this_thread::get_id()); }
void td_json_client_send(void*, const char*) { assert(td_thread==std::this_thread::get_id()); ++sent; }
const char* td_json_client_receive(void*, double) {
    assert(td_thread==std::this_thread::get_id()); ++receive_count;
    static thread_local std::string response;
    if (produced >= 6000) return nullptr;
    int n=++produced;
    if (n%10==0) response="{\"@type\":\"updateUserStatus\",\"user_id\":"+std::to_string(n)+",\"status\":{\"@type\":\"userStatusOnline\",\"expires\":2000000000}}";
    else response="{\"@type\":\"updateFile\",\"file\":{\"id\":10,\"remote\":{\"uploaded_size\":"+std::to_string(n)+"}}}";
    return response.c_str();
}
void td_set_log_message_callback(int, td_log_message_callback_ptr) {}
}
static json_t* parse(const char* text) { json_error_t e{}; auto* r=json_loads(text,0,&e); assert(r); return r; }
static void event(MediaUploads& u, const char* text, int64_t now) { auto* r=parse(text); u.process(r,now); json_decref(r); }
int main() {
    MediaUploads u;
    json_decref(u.begin("video.mp4",10000,true,1,1000000));
    json_decref(u.begin("audio.ogg",2000,false,2,1000000));
    event(u,R"({"@type":"updateFile","file":{"id":10,"size":10000,"local":{"path":"video.mp4"},"remote":{"uploaded_size":2500,"is_uploading_active":true}}})",2000000);
    assert(u.items()[0].done==2500 && u.items()[0].speed==2500);
    assert(u.items()[1].done==0);
    assert(u.summary(2000000).find("25%")!=std::string::npos);
    assert(u.summary(9000000).find("нет прогресса 7 с")!=std::string::npos);
    event(u,R"({"@type":"message","chat_id":1,"id":101,"@extra":{"kind":"media_upload","token":1},"content":{"video_note":{"video":{"id":10,"size":10000,"remote":{"uploaded_size":2500,"is_uploading_active":true}}}}})",3000000);
    event(u,R"({"@type":"message","chat_id":2,"id":101,"@extra":{"kind":"media_upload","token":2},"content":{"voice_note":{"voice":{"id":11,"size":2000,"remote":{"uploaded_size":2000,"is_uploading_completed":true}}}}})",3000000);
    assert(!u.items()[1].terminal && u.items()[1].uploaded);
    event(u,R"({"@type":"updateMessageSendSucceeded","old_message_id":101,"message":{"chat_id":2,"id":900}})",4000000);
    assert(!u.items()[0].terminal && u.items()[1].terminal);
    event(u,R"({"@type":"updateFile","file":{"id":10,"remote":{"uploaded_size":0,"is_uploading_active":true}}})",4000000);
    assert(u.items()[0].speed==0); // Retry does not produce a negative ETA.
    event(u,R"({"@type":"updateMessageSendFailed","old_message_id":101,"message":{"chat_id":1},"error":{"message":"NETWORK_ERROR"}})",5000000);
    assert(u.items()[0].terminal && u.items()[0].error=="NETWORK_ERROR");
    assert(u.lines(21000000).empty());
    json_decref(u.begin("other.mp4",1000,true,3,22000000));
    event(u,R"({"@type":"error","@extra":{"kind":"media_upload","token":3},"message":"FILE_ERROR"})",23000000);
    assert(u.items().back().error=="FILE_ERROR");

    // Run the production TD worker/parser while UI consumption is deliberately stopped.
    // The old 128+128 caps stop receive() long before all these updates are delivered.
    TelegramClient tg;
    tg.worker_started_=true; tg.parser_started_=true;
    assert(!pthread_create(&tg.worker_,nullptr,TelegramClient::workerEntry,&tg));
    assert(!pthread_create(&tg.parser_,nullptr,TelegramClient::parserEntry,&tg));
    auto deadline=sceKernelGetSystemTimeWide()+5000000;
    while (produced<6000 && sceKernelGetSystemTimeWide()<deadline) sceKernelDelayThread(1000);
    assert(produced==6000);
    for (;;) {
        pthread_mutex_lock(&tg.queue_mutex_);
        bool empty=tg.incoming_.empty();
        size_t count=tg.parsed_.size();
        pthread_mutex_unlock(&tg.queue_mutex_);
        if (empty && count==601) break;
        assert(sceKernelGetSystemTimeWide()<deadline); sceKernelDelayThread(1000);
    }
    int before=receive_count; sceKernelDelayThread(10000); assert(receive_count>before);
    // Queue a new request while congested; once the UI drains, it must resume.
    tg.sendRaw("{\"@type\":\"getChat\",\"chat_id\":1}");
    while (sent==0 && sceKernelGetSystemTimeWide()<deadline) { tg.pump(); sceKernelDelayThread(1000); }
    assert(sent>0);
    while (tg.user_presence_.size()<600 && sceKernelGetSystemTimeWide()<deadline) tg.pump();
    assert(tg.user_presence_.size()==600); // No control updates lost during progress coalescing.
    tg.stopWorker();

    // Production request builders preserve original chat/topic and unique correlation.
    FILE* f=std::fopen("upload-test.bin","wb"); assert(f); std::fputs("test",f); std::fclose(f);
    tg.worker_started_=true; tg.ready_=true; tg.current_chat_id_=999; tg.current_topic_=999;
    assert(tg.sendAudioFile("upload-test.bin",2,42,7));
    assert(tg.sendVideoNoteFile("upload-test.bin",2,43,0));
    assert(tg.outgoing_.size()==2);
    auto* audio=parse(tg.outgoing_[0].text.c_str()); auto* video=parse(tg.outgoing_[1].text.c_str());
    assert(MediaUploads::number(audio,"chat_id")==42 && MediaUploads::number(video,"chat_id")==43);
    assert(MediaUploads::number(json_object_get(audio,"topic_id"),"forum_topic_id")==7);
    assert(json_is_null(json_object_get(video,"topic_id")));
    assert(MediaUploads::number(json_object_get(audio,"@extra"),"token") != MediaUploads::number(json_object_get(video,"@extra"),"token"));
    auto* content=json_object_get(audio,"input_message_content");
    assert(MediaUploads::string(content,"@type")=="inputMessageVoiceNote");
    assert(MediaUploads::string(json_object_get(content,"voice_note"),"@type")=="inputVoiceNote");
    json_decref(audio); json_decref(video);
    tg.worker_started_=false; std::remove("upload-test.bin");
    std::puts("Transfers: production workers, backpressure, progress, parallel sends and destinations passed");
}
