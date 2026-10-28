#include "test_language.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <chrono>
#include <future>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <jansson.h>
#define private public
#include "telegram_client.h"
#undef private
#include "td_event_queue.h"
#include "td_sticker_sets.h"
#include "performance.h"
#include "diag_log.h"
#include "account_cleanup.h"
#include "sticker_preview.h"
#include "transient_status.h"
#include "file_download.h"
#include "video_quality.h"
#include <unistd.h>
#include "cpu_scheduler.h"
#include <psp2/io/stat.h>
#include <td/telegram/td_json_client.h>

int64_t sceKernelGetSystemTimeWide() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int sceKernelDelayThread(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); return 0; }
int sceIoGetstat(const char* p, SceIoStat* out) { struct stat s{}; int rc = stat(p, &s); out->st_size=s.st_size; out->st_mode=s.st_mode; return rc; }
static std::atomic<bool> export_blocked{false}, export_entered{false}, export_released{false};
int sceIoMkdir(const char* p, int m) {
    if (export_blocked && !std::strcmp(p, "ux0:data/downloads")) {
        export_entered = true;
        while (!export_released) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return mkdir(p, m);
}
namespace cpu_scheduler {
Thread::Thread(Role):id_(0) {} Thread::~Thread() {}
Media::Media(Role r):role_(r) {} Media::~Media() {}
bool videoActive() { return false; }
}
static std::atomic<int> receive_count{0}, produced{0}, sent{0};
static constexpr const char* slow_media_path = "ux0:data/vitagram/files/slow-startup.bin";
static std::atomic<bool> storage_blocked{false}, storage_entered{false}, storage_released{false};
static std::atomic<bool> full_cache_blocked{false};
static std::atomic<int> quality_opens{0}, quality_stats{0};
static std::atomic<bool> quality_blocked{false}, quality_entered{false}, quality_released{false};
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    if (!std::strcmp(path, "ux0:data/vitagram/video-quality.log")) {
        ++quality_opens;
        if (quality_blocked) {
            quality_entered = true;
            while (!quality_released) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return __real_fopen(path, mode);
}
extern "C" int __real_stat(const char*, struct stat*);
extern "C" int __wrap_stat(const char* name, struct stat* out) {
    if (!std::strcmp(name, "ux0:data/vitagram/video-quality.log")) ++quality_stats;
    if ((storage_blocked && !std::strcmp(name, slow_media_path)) ||
        (full_cache_blocked && !std::strcmp(name, VITAGRAM_DB_DIR "/ui-cache.json"))) {
        storage_entered = true;
        while (!storage_released) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return __real_stat(name, out);
}
static std::thread::id td_thread;
static bool td_logging_configured = false;
extern "C" {
const char* td_json_client_execute(void*, const char* request) {
    assert(!std::strcmp(request, "{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":1}"));
    td_thread = std::this_thread::get_id(); td_logging_configured = true;
    return "{\"@type\":\"ok\"}";
}
void* td_json_client_create() {
    assert(td_logging_configured && td_thread == std::this_thread::get_id());
    td_logging_configured = false; return &sent;
}
void td_json_client_destroy(void*) { assert(td_thread==std::this_thread::get_id()); }
static std::atomic<int> tracked_sends{0};
void td_json_client_send(void*, const char* text) {
    assert(td_thread==std::this_thread::get_id()); ++sent;
    auto* root=json_loads(text,0,nullptr);assert(root);
    auto* extra=json_object_get(root,"@extra");
    const char* kind=json_string_value(json_object_get(extra,"kind"));
    if (kind && ((!std::strcmp(kind,"history") && json_is_true(json_object_get(extra,"first"))) || !std::strcmp(kind,"forum_check"))) {
        assert(json_integer_value(json_object_get(extra,"selected_at"))>0);
        assert(json_integer_value(json_object_get(extra,"send_at"))>=json_integer_value(json_object_get(extra,"selected_at")));
        ++tracked_sends;
    }
    json_decref(root);
}
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
    use_test_language();

    {
        TelegramClient client;
        client.optimizeCache(30, 512, true);
        assert(client.outgoing_.empty() && !client.cacheMaintenance());
        client.ready_ = true; client.current_chat_id_ = 42; client.worker_started_ = true;
        client.optimizeCache(30, 512);
        assert(client.outgoing_.empty()); // Automatic cleanup still waits until chats close.
        for (int days : {1, 7, 30, 90, 0}) for (int mb : {128, 256, 512, 1024, 2048, 0}) {
            client.optimizeCache(days, mb, true);
            assert(client.cacheMaintenance() && client.cacheCleanupError().empty());
            assert(client.outgoing_.size() == 1);
            auto* request = parse(client.outgoing_.back().text.c_str());
            assert(!std::strcmp(json_string_value(json_object_get(request, "@type")), "optimizeStorage"));
            assert(json_integer_value(json_object_get(request, "ttl")) == (days ? days * 86400 : INT32_MAX));
            assert(json_integer_value(json_object_get(request, "size")) == (mb ? (json_int_t)mb * 1024 * 1024 : ((json_int_t)1 << 52)));
            assert(json_array_size(json_object_get(request, "file_types")) == 10);
            json_decref(request);
            client.optimizeCache(1, 128, true);
            assert(client.outgoing_.size() == 1); // No duplicate operation while one is pending.
            client.outgoing_.clear();
            client.process(parse(R"({"@type":"storageStatistics","@extra":"cache_cleanup"})"));
            assert(!client.cacheMaintenance() && client.cacheCleanupError().empty());
        }
        client.optimizeCache(7, 256, true);
        client.outgoing_.clear();
        client.process(parse(R"({"@type":"error","message":"Offline","@extra":"cache_cleanup"})"));
        assert(!client.cacheMaintenance() && client.cacheCleanupError() == "Offline");
        client.optimizeCache(90, 1024, true);
        assert(client.cacheMaintenance() && client.cacheCleanupError().empty());
        client.process(parse(R"({"@type":"storageStatistics","@extra":"cache_cleanup"})"));
        client.worker_started_ = false;
        std::puts("Cache cleanup: current limits, unlimited settings, open chat, offline, duplicate, failure and retry passed");
    }

    {
        TelegramClient client;
        client.current_chat_id_ = 42;
        for (int i=0;i<8;++i) {
            MessageItem track; track.id=i+1; track.voice=true; track.audio=true;
            track.voice_file_id=100+i; track.voice_duration=60; track.voice_path="local.mp3";
            client.messages_.push_back(track);
        }
        client.startAudioQueue(3);
        assert(client.audio_queue_.size()==8 && client.audioTrack()->file==103);
        assert(client.advanceAudioQueue(-1,false) && client.audioTrack()->file==102);
        assert(client.advanceAudioQueue() && client.audioTrack()->file==103);
        assert(client.audioTrackIndex(4)==3);
        client.current_chat_id_=77;
        assert(client.audioTrackIndex(4)==-1 && client.audioTrack()->file==103);
        client.current_chat_id_=42;
        client.shuffleAudioQueue();
        assert(client.audioShuffle() && client.audioTrack()->file==103);
        std::unordered_set<int> visited{103};
        while (client.advanceAudioQueue()) assert(visited.insert(client.audioTrack()->file).second);
        assert(visited.size()==8);
        const auto last=client.audioTrack()->file;
        assert(!client.advanceAudioQueue() && client.audioTrack()->file==last);
        assert(client.advanceAudioQueue(-1,false));
        assert(client.advanceAudioQueue(1,false) && client.audioTrack()->file==last);
        client.cycleAudioRepeat();
        assert(client.audioRepeat()==1 && client.advanceAudioQueue() && client.audioTrack()->file==103);
        assert(client.advanceAudioQueue(-1,false) && client.audioTrack()->file==last);
        client.cycleAudioRepeat();
        assert(client.audioRepeat()==2 && client.advanceAudioQueue() && client.audioTrack()->file==last);
        client.shuffleAudioQueue();
        assert(!client.audioShuffle() && client.audioTrack()->file==last);
        for (size_t i=1;i<client.audio_queue_.size();++i) assert(client.audio_queue_[i-1].message<client.audio_queue_[i].message);
        client.cycleAudioRepeat(); assert(client.audioRepeat()==0);
        client.startAudioQueue(0);
        assert(!client.advanceAudioQueue(-1,false));
        for (auto& track:client.audio_queue_) { track.path.clear(); client.pending_files_.insert(track.file); }
        client.serviceAudioQueue();
        assert(client.pending_files_.size()==2 && client.pending_files_.count(100) && client.pending_files_.count(101));
        client.clearAudioQueue();
        assert(!client.audioTrack() && !client.advanceAudioQueue() && client.pending_files_.empty());
        client.messages_[3].voice_error="unavailable";
        client.startAudioQueue(3); assert(!client.audioTrack());
        client.messages_[3].voice_error.clear();
        client.messages_[1].voice_file_id=client.messages_[0].voice_file_id;
        client.startAudioQueue(1);
        client.shuffleAudioQueue(); client.shuffleAudioQueue();
        assert(client.audioTrack()->message==2);
        for (int i=8;i<100;++i) {
            auto track=client.messages_[0]; track.id=i+1; track.voice_file_id=100+i;
            client.messages_.push_back(track);
        }
        client.startAudioQueue(80);
        assert(client.audioTrack()->file==180 && client.audio_queue_.size()<=64);
        assert(client.advanceAudioQueue(-1,false) && client.audioTrack()->file==179);
        client.startAudioQueue(0); assert(client.audio_queue_.size()==64);
    }

    {
        TelegramClient client;
        client.ready_ = true;
        ChatItem chat; chat.id = 42; chat.user_id = 77;
        client.known_chats_.push_back(chat);
        client.current_chat_id_ = 42;
        assert(client.botMenu().empty());
        assert(!client.followingBotReplies() && !client.takeBotReplyFocus());
        auto update = [&](const char* wire) { client.process(parse(wire)); };
        update(R"({"@type":"updateUserFullInfo","user_id":77,"user_full_info":{"bot_info":{"commands":[{"command":"help","description":"Помощь"}]}}})");
        assert(client.botMenu().rows.size() == 1);
        assert(client.botMenu().rows[0][0].value == "/help");
        client.worker_started_ = true;
        assert(client.activateBotButton(0, 0, 0, false));
        assert(client.followingBotReplies() && client.takeBotReplyFocus() && !client.takeBotReplyFocus());
        client.worker_started_ = false;
        assert(client.outgoing_.size() == 1);
        auto* command_request = parse(client.outgoing_.back().text.c_str());
        assert(!std::strcmp(json_string_value(json_object_get(command_request, "@type")), "sendMessage"));
        assert(json_integer_value(json_object_get(command_request, "chat_id")) == 42);
        assert(!std::strcmp(json_string_value(json_object_get(json_object_get(json_object_get(command_request, "input_message_content"), "text"), "text")), "/help"));
        json_decref(command_request); client.outgoing_.clear();
        assert(client.inputKind() == InputKind::None);
        assert(!client.activateBotButton(0, -1, 0, false));
        assert(!client.activateBotButton(0, 1, 0, false));
        assert(!client.takeBotReplyFocus());
        update(R"({"@type":"updateChatReplyMarkup","chat_id":42,"reply_markup_message":{"id":123,"reply_markup":{"@type":"replyMarkupShowKeyboard","one_time":true,"rows":[[{"text":"Новости","type":{"@type":"keyboardButtonTypeText"}}]]}}})");
        assert(client.botKeyboard().id == 123 && !client.botKeyboard().buttons.empty());
        assert(!client.activateBotButton(122, 0, 0, true));
        client.worker_started_ = true;
        assert(client.activateBotButton(123, 0, 0, true));
        assert(client.followingBotReplies() && client.takeBotReplyFocus());
        client.worker_started_ = false;
        assert(client.outgoing_.size() == 1);
        auto* keyboard_request = parse(client.outgoing_.back().text.c_str());
        assert(!std::strcmp(json_string_value(json_object_get(keyboard_request, "@type")), "sendMessage"));
        assert(!std::strcmp(json_string_value(json_object_get(json_object_get(json_object_get(keyboard_request, "input_message_content"), "text"), "text")), "Новости"));
        json_decref(keyboard_request); client.outgoing_.clear();
        assert(!client.botKeyboard().buttons.empty()); // One-time hides the panel; the keyboard remains reopenable.
        update(R"({"@type":"updateChatReplyMarkup","chat_id":42,"reply_markup_message":null})");
        assert(client.botKeyboard().buttons.empty());
        update(R"({"@type":"message","chat_id":42,"id":321,"content":{"@type":"messageText","text":{"text":"Choose"}},"reply_markup":{"@type":"replyMarkupInlineKeyboard","rows":[[{"text":"Callback","type":{"@type":"inlineKeyboardButtonTypeCallback","data":"AP8="}},{"text":"Link","type":{"@type":"inlineKeyboardButtonTypeUrl","url":"https://example.org"}}]]}})");
        assert(client.messages().size() == 1);
        assert(client.messages()[0].buttons.rows[0][0].value == "AP8=");
        assert(client.activateBotButton(321, 0, 0, false));
        assert(client.takeBotReplyFocus());
        assert(client.activateBotButton(321, 0, 1, false));
        assert(!client.takeBotReplyFocus());
        assert(client.takeBotLink() == "https://example.org");
        assert(client.takeBotLink().empty());
        update(R"({"@type":"callbackQueryAnswer","text":"Long answer","show_alert":true,"@extra":{"kind":"bot_action","chat":42,"generation":0}})");
        assert(client.takeBotAlert() == "Long answer");
        assert(client.takeBotAlert().empty());
        update(R"({"@type":"callbackQueryAnswer","text":"Stale","show_alert":true,"url":"https://example.org/stale","@extra":{"kind":"bot_action","chat":43,"generation":0}})");
        assert(client.takeBotAlert().empty() && client.takeBotLink().empty());
        update(R"({"@type":"callbackQueryAnswer","text":"Other topic","show_alert":true,"@extra":{"kind":"bot_action","chat":42,"generation":0,"topic":123}})");
        assert(client.takeBotAlert().empty());
        update(R"({"@type":"updateMessageEdited","chat_id":42,"message_id":321,"edit_date":1,"reply_markup":null})");
        update(R"({"@type":"updateNewMessage","message":{"chat_id":42,"id":400,"content":{"@type":"messageText","text":{"text":"Bot result"}}}})");
        assert(client.messages().back().id == 400 && client.followingBotReplies());
        assert(client.messages()[0].buttons.empty());
        assert(!client.activateBotButton(321, 0, 0, false));
        assert(!client.takeBotReplyFocus());
        update(R"({"@type":"updateUserFullInfo","user_id":77,"user_full_info":{"bot_info":{"commands":[]}}})");
        assert(client.botMenu().empty());
        update(R"({"@type":"updateUserFullInfo","user_id":77,"user_full_info":{"bot_info":{"menu_button":{"text":"App","url":"https://example.org/app"},"commands":[{"command":"help","description":"Help"}]}}})");
        assert(client.botMenu().rows[0][0].type == "inlineKeyboardButtonTypeWebApp");
        assert(client.botMenu().rows.size() == 2 && client.botMenu().rows[1][0].value == "/help");
        assert(client.activateBotButton(0, 0, 0, false));
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Confirm);
        assert(client.botWebApp().bot == 77 && !client.botWebApp().keyboard);
        auto* theme = parse(R"({"@type":"themeParameters","background_color":0})");
        client.openBotWebApp(theme); json_decref(theme);
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Loading);
        const auto generation = client.bot_webapp_generation_;
        auto* response = parse(R"({"@type":"webAppInfo","launch_id":"10001","url":{"@type":"webAppUrl","url":"https://example.org/app#signed","require_same_origin":true}})");
        json_object_set_new(response, "@extra", json_pack("{s:s,s:I}", "kind", "bot_webapp", "generation", (json_int_t)generation));
        client.process(response);
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Ready && client.botWebApp().launch == 10001);
        assert(client.botWebApp().same_origin && !client.sendBotWebAppData("data"));
        client.closeBotWebApp();
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Closed);
        update(R"({"@type":"webAppUrl","url":"https://example.org/stale","@extra":{"kind":"bot_webapp","generation":0}})");
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Closed);
        update(R"({"@type":"updateChatReplyMarkup","chat_id":42,"reply_markup_message":{"id":456,"sender_id":{"@type":"messageSenderUser","user_id":77},"reply_markup":{"@type":"replyMarkupShowKeyboard","rows":[[{"text":"App","type":{"@type":"keyboardButtonTypeWebApp","url":"https://example.org"}}]]}}})");
        assert(client.activateBotButton(456, 0, 0, true));
        assert(client.botWebApp().keyboard);
        client.bot_webapp_.phase = TelegramClient::BotWebApp::Ready;
        assert(!client.sendBotWebAppData(std::string(4097, 'x')));
        assert(!client.sendBotWebAppData(""));
        assert(!client.sendBotWebAppData(std::string("prefix\0suffix", 13)));
        assert(client.sendBotWebAppData("emoji 👩‍🚀 and \\\"quotes\\\""));
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Closed);
        update(R"({"@type":"error","code":400,"message":"DATA_INVALID","@extra":{"kind":"bot_webapp_data","chat":42,"generation":0}})");
        assert(client.takeBotAlert() == "DATA_INVALID");
        update(R"({"@type":"user","id":77,"restriction_info":{"@type":"restrictionInfo"},"type":{"@type":"userTypeBot"}})");
        assert(!client.activateBotButton(0, 0, 0, false));
        assert(client.takeBotLink().empty());
        client.focusBotReplies();
        client.leaveChat();
        assert(!client.followingBotReplies() && !client.takeBotReplyFocus());
        assert(client.botMenu().empty() && client.botKeyboard().buttons.empty());
        std::puts("Bots: availability, removal, one-time keyboard, callback data, URL, edits, stale source and chat exit passed");
    }
    {
        TelegramClient client;
        client.ready_ = client.worker_started_ = true;
        client.current_chat_id_ = 42;
        client.history_window_ = true;
        MessageItem button; button.id = 10;
        button.buttons.rows = {{{"Run", "inlineKeyboardButtonTypeCallback", "data"}}};
        client.messages_.push_back(button);
        assert(client.activateBotButton(10, 0, 0, false));
        assert(!client.historyWindow() && client.followingBotReplies() && client.takeBotReplyFocus());
        bool requested_latest = false;
        for (const auto& outgoing : client.outgoing_) {
            auto* request = parse(outgoing.text.c_str());
            if (!std::strcmp(json_string_value(json_object_get(request, "@type")), "getChatHistory")) {
                assert(json_integer_value(json_object_get(request, "from_message_id")) == 0);
                requested_latest = true;
            }
            json_decref(request);
        }
        assert(requested_latest);
        client.process(parse(R"({"@type":"updateNewMessage","message":{"chat_id":42,"id":500,"content":{"@type":"messageText","text":{"text":"Fresh bot result"}}}})"));
        assert(client.messages().back().id == 500 && client.followingBotReplies());
        client.focusBotReplies();
        ++client.history_generation_;
        assert(!client.followingBotReplies() && !client.takeBotReplyFocus());
        client.worker_started_ = false;
        std::puts("Bot focus: commands, replies, callbacks, latest history and stale generations passed");
    }
    {
        TelegramClient client;
        client.ready_ = client.worker_started_ = true;
        client.current_chat_id_ = 42;
        auto request = [&]() { assert(!client.outgoing_.empty()); return parse(client.outgoing_.back().text.c_str()); };
        auto reply = [&](const char* body) {
            auto* req = request();
            auto* response = parse(body);
            json_object_set(response, "@extra", json_object_get(req, "@extra"));
            json_decref(req); client.outgoing_.clear(); client.process(response);
        };
        auto main_link = [&] {
            MessageItem message; message.id = 100;
            message.buttons.rows = {{{"Open", "inlineKeyboardButtonTypeUrl", "https://t.me/example_bot?startapp=preserve_me&mode=compact"}}};
            client.messages_ = {message};
            assert(client.activateBotButton(100, 0, 0, false));
            assert(client.openTelegramLink(client.takeBotLink()));
            auto* req = request(); assert(MediaUploads::string(req, "@type") == "getInternalLinkType"); json_decref(req);
            reply(R"({"@type":"internalLinkTypeMainWebApp","bot_username":"example_bot","start_parameter":"preserve_me","mode":{"@type":"webAppOpenModeCompact"}})");
            req = request(); assert(MediaUploads::string(req, "@type") == "searchPublicChat"); json_decref(req);
            reply(R"({"@type":"chat","id":77,"type":{"@type":"chatTypePrivate","user_id":77}})");
        };
        main_link();
        reply(R"({"@type":"user","id":77,"first_name":"Bot","type":{"@type":"userTypeBot","has_main_web_app":true}})");
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Confirm && client.currentChatId() == 42);
        assert(client.outgoing_.empty()); // No signed URL or write permission before consent.
        auto* theme = parse(R"({"@type":"themeParameters","background_color":0})");
        client.openBotWebApp(theme);
        auto* req = request();
        assert(MediaUploads::string(req, "@type") == "getMainWebApp");
        assert(MediaUploads::string(req, "start_parameter") == "preserve_me");
        assert(MediaUploads::number(req, "bot_user_id") == 77 && MediaUploads::number(req, "chat_id") == 42);
        assert(MediaUploads::string(json_object_get(json_object_get(req,"parameters"),"mode"),"@type") == "webAppOpenModeCompact");
        assert(!json_object_get(req,"url")); json_decref(req);
        reply(R"({"@type":"mainWebApp","url":{"@type":"webAppUrl","url":"https://example.org/#signed","require_same_origin":true},"mode":{"@type":"webAppOpenModeCompact"}})");
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Ready && client.botWebApp().same_origin);
        assert(client.botWebApp().signed_url == "https://example.org/#signed");

        assert(client.openTelegramLink("https://t.me/example_bot/shop?startapp=order"));
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Closed); // New navigation cancels the old session.
        reply(R"({"@type":"internalLinkTypeWebApp","bot_username":"example_bot","web_app_short_name":"shop","start_parameter":"order","mode":{"@type":"webAppOpenModeFullSize"}})");
        reply(R"({"@type":"chat","id":77,"type":{"@type":"chatTypePrivate","user_id":77}})");
        reply(R"({"@type":"user","id":77,"type":{"@type":"userTypeBot"}})");
        req = request(); assert(MediaUploads::string(req,"@type") == "searchWebApp"); json_decref(req);
        reply(R"({"@type":"foundWebApp","web_app":{"@type":"webApp","short_name":"shop","title":"Shop"},"request_write_access":true,"skip_confirmation":false})");
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Confirm && client.botWebApp().label == "Shop");
        client.openBotWebApp(theme);
        req = request();
        assert(MediaUploads::string(req,"@type") == "getWebAppLinkUrl");
        assert(MediaUploads::string(req,"web_app_short_name") == "shop" && MediaUploads::string(req,"start_parameter") == "order");
        assert(json_is_false(json_object_get(req,"allow_write_access"))); json_decref(req);
        reply(R"({"@type":"webAppUrl","url":"https://example.org/shop#signed","require_same_origin":false})");
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Ready && !client.botWebApp().same_origin);

        main_link();
        client.closeBotWebApp();
        reply(R"({"@type":"user","id":77,"type":{"@type":"userTypeBot","has_main_web_app":true}})");
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Closed && client.outgoing_.empty());
        main_link();
        reply(R"({"@type":"user","id":77,"type":{"@type":"userTypeRegular"}})");
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Failed && client.outgoing_.empty());
        main_link();
        reply(R"({"@type":"user","id":77,"restriction_info":{"@type":"restrictionInfo"},"type":{"@type":"userTypeBot","has_main_web_app":true}})");
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Failed && client.outgoing_.empty());
        main_link();
        reply(R"({"@type":"user","id":77,"type":{"@type":"userTypeBot","has_main_web_app":false}})");
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Failed && client.outgoing_.empty());
        main_link();
        reply(R"({"@type":"error","code":400,"message":"USER_NOT_FOUND"})");
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Failed && client.outgoing_.empty());

        assert(client.openTelegramLink("https://t.me/example_bot?startapp=old"));
        req = request(); auto* old = parse(R"({"@type":"internalLinkTypeMainWebApp","bot_username":"example_bot","mode":{"@type":"webAppOpenModeFullSize"}})");
        json_object_set(old,"@extra",json_object_get(req,"@extra")); json_decref(req);
        client.openChat(43,"Other"); client.outgoing_.clear(); client.process(old);
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Closed && client.outgoing_.empty());
        assert(client.openTelegramLink("https://t.me/example_bot?start=ordinary"));
        reply(R"({"@type":"internalLinkTypeBotStart","bot_username":"example_bot","start_parameter":"ordinary"})");
        req = request(); assert(MediaUploads::string(req,"@type") == "searchPublicChat"); json_decref(req);
        client.outgoing_.clear();
        assert(!client.openTelegramLink("https://evil.test/t.me/example_bot?startapp=x"));
        assert(!client.openTelegramLink("https://t.me/example_bot?startapp=x\n"));
        assert(!client.openTelegramLink(std::string("https://t.me/example_bot?startapp=x") + '\0' + "suffix"));
        assert(client.openTelegramLink("https://t.me/example_bot?startapp=x"));
        reply(R"({"@type":"internalLinkTypeMainWebApp","bot_username":"example_bot","mode":{"@type":"invalid"}})");
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Failed && client.outgoing_.empty());
        assert(client.openTelegramLink("tg://resolve?domain=example_bot&startapp=deep"));
        req = request(); assert(MediaUploads::string(req,"@type") == "getInternalLinkType"); json_decref(req);
        reply(R"({"@type":"internalLinkTypeMainWebApp","bot_username":"example_bot","start_parameter":"deep","mode":{"@type":"webAppOpenModeFullScreen"}})");
        reply(R"({"@type":"chat","id":77,"type":{"@type":"chatTypePrivate","user_id":77}})");
        reply(R"({"@type":"user","id":77,"type":{"@type":"userTypeBot","has_main_web_app":true}})");
        assert(client.botWebApp().phase == TelegramClient::BotWebApp::Confirm && client.botWebApp().start_parameter == "deep");
        assert(client.botWebApp().mode == "webAppOpenModeFullScreen");
        json_decref(theme); client.worker_started_ = false;
        std::puts("Mini App links: main/named apps, bot validation, consent, parameters, signed URLs, cancellation, stale replies and ordinary links passed");
    }
    {
        TelegramClient client;
        auto colors = parse(R"({"@type":"updateAccentColors","colors":[{"id":8,"light_theme_colors":[1122867,4478310,7833753],"dark_theme_colors":[11189196,14544639]}]})");
        client.process(colors);
        auto user = parse(R"({"@type":"user","id":77,"first_name":"Original","accent_color_id":8})");
        client.process(user);
        auto palette = client.senderColors(77, false, false);
        assert(palette.count == 3 && palette.rgb[0] == 0x112233 && palette.rgb[2] == 0x778899);
        assert(client.senderColors(77, false, true).rgb[0] == 0xaabbcc);
        assert(peer_colors::rgba(0x112233) == 0xff332211u);
        assert(client.senderColors(1, false, false).rgb[0] == 0xd67722);
        auto* invalid = parse(R"([-1,16777216])");
        assert(!peer_colors::read(invalid).count); json_decref(invalid);
        auto* gift = parse(R"({"@type":"user","id":78,"accent_color_id":8,"upgraded_gift_colors":{"light_theme_accent_color":66051,"dark_theme_accent_color":263430}})");
        client.process(gift);
        assert(client.senderColors(78, false, false).rgb[0] == 0x010203);
        assert(client.senderColors(78, false, true).rgb[0] == 0x040506);
        client.process(parse(R"({"@type":"chat","id":-1000000000123,"title":"Channel","accent_color_id":8,"positions":[{"list":{"@type":"chatListMain"},"order":100}]})"));
        assert(client.senderColors(-1000000000123LL, true, false).count == 3);
        client.process(parse(R"({"@type":"updateChatAccentColors","chat_id":-1000000000123,"accent_color_id":3,"upgraded_gift_colors":null})"));
        assert(client.senderColors(-1000000000123LL, true, false).rgb[0] == 0x40a920);
        client.current_chat_id_ = 42;
        auto* raw = parse(R"({"id":1,"sender_id":{"@type":"messageSenderUser","user_id":77},"content":{"@type":"messageText","text":{"text":"Original"}}})");
        client.history_items_.push_back(client.parseMessage(raw)); json_decref(raw);
        raw = parse(R"({"id":2,"sender_id":{"@type":"messageSenderUser","user_id":78},"reply_to":{"@type":"messageReplyToMessage","chat_id":42,"message_id":1},"content":{"@type":"messageText","text":{"text":"Reply"}}})");
        client.messages_.push_back(client.parseMessage(raw)); json_decref(raw);
        client.resolveReplies(false);
        assert(client.messages_[0].reply_sender_id == 77 && !client.messages_[0].reply_sender_chat);
        assert(client.messages_[0].reply_sender == "Original");
        client.history_items_.clear();
        client.reply_cache_[{42, 1}].sender_id = 77;
        client.reply_cache_[{42, 1}].text = "Cached";
        client.resolveReplies(false);
        assert(client.messages_[0].reply_sender_id == 77 && client.messages_[0].reply_text == "Cached");
        raw = parse(R"({"id":3,"reply_to":{"@type":"messageReplyToMessage","chat_id":99,"message_id":5,"origin":{"@type":"messageOriginChannel","chat_id":-1000000000123},"content":{"@type":"messageText","text":{"text":"Outside history"}}},"content":{"@type":"messageText","text":{"text":"Reply"}}})");
        client.messages_.push_back(client.parseMessage(raw)); json_decref(raw);
        client.resolveReplies(false);
        assert(client.messages_[1].reply_sender_id == -1000000000123LL && client.messages_[1].reply_sender_chat);
        raw = parse(R"({"id":4,"reply_to":{"@type":"messageReplyToMessage","message_id":9,"quote":{"text":{"text":"Selected quote"}}},"content":{"@type":"messageText","text":{"text":"Reply"}}})");
        client.messages_.push_back(client.parseMessage(raw)); json_decref(raw);
        client.ready_ = client.worker_started_ = true;
        client.resolveReplies(true);
        assert(client.reply_requested_.count({42, 9})); // A quote still needs the original author's identity.
        client.worker_started_ = client.ready_ = false;
        auto* snapshot = client.ui_cache_.startupSnapshot();
        TelegramClient restored; restored.applyRestoredCache(snapshot);
        assert(restored.senderColors(77, false, false).rgb[0] == 0x112233);
        assert(restored.senderColors(78, false, true).rgb[0] == 0x040506);
        assert(restored.senderColors(-1000000000123LL, true, false).rgb[0] == 0x40a920);
        // A delayed cache snapshot cannot replace newer live palettes or users.
        restored.process(parse(R"({"@type":"updateAccentColors","colors":[{"id":8,"light_theme_colors":[7]}]})"));
        restored.applyRestoredCache(client.ui_cache_.startupSnapshot());
        assert(restored.senderColors(77, false, false).rgb[0] == 7);
        restored.process(parse(R"({"@type":"updateAccentColors","colors":[]})"));
        assert(restored.senderColors(77, false, false).rgb[0] == 0xcc5049); // 77 % 7
        std::puts("Telegram colors: palettes, gifts, reply authors, quoted lookups, cache and live precedence passed");
    }

    {
        TelegramClient client;
        auto* raw = parse(R"({"id":90,"content":{"@type":"messageText","text":{"text":"😀Тест","entities":[{"offset":2,"length":4,"type":{"@type":"textEntityTypeBold"}},{"offset":2,"length":4,"type":{"@type":"textEntityTypeSpoiler"}},{"offset":99,"length":1,"type":{"@type":"textEntityTypeItalic"}}]}}})");
        auto item = client.parseMessage(raw);
        assert(item.styles.size() == 2);
        assert(item.styles[0].begin == 4 && item.styles[0].end == 12 && item.styles[0].style == TextBold);
        assert(item.styles[1].begin == 4 && item.styles[1].end == 12 && item.styles[1].style == TextSpoiler);
        const char* kinds[] = {"Bold", "Italic", "Underline", "Strikethrough", "Spoiler", "Code", "Pre", "PreCode", "BlockQuote", "ExpandableBlockQuote"};
        const unsigned flags[] = {TextBold, TextItalic, TextUnderline, TextStrike, TextSpoiler, TextCode, TextCode, TextCode, TextQuote, TextQuote};
        auto* entities = json_object_get(json_object_get(json_object_get(raw, "content"), "text"), "entities");
        for (size_t i = 0; i < 10; ++i) {
            json_array_clear(entities);
            const auto kind = std::string("textEntityType") + kinds[i];
            json_array_append_new(entities, json_pack("{s:i,s:i,s:{s:s}}", "offset", 2, "length", 4, "type", "@type", kind.c_str()));
            const auto styled = client.parseMessage(raw);
            assert(styled.styles.size() == 1 && styled.styles[0].style == flags[i]);
        }
        json_decref(raw);
    }
    {
        TelegramClient previews;
        const std::string content = R"({"@type":"messageText","text":{"text":"https://example.org","entities":[{"offset":0,"length":19,"type":{"@type":"textEntityTypeUrl"}}]},"link_preview":{"url":"https://example.org","site_name":"Example","title":"Article","description":{"text":"Описание"},"type":{"@type":"linkPreviewTypeArticle","photo":{"sizes":[{"width":320,"height":180,"photo":{"id":71,"local":{"path":"preview.jpg","is_downloading_completed":true}}}]}}}})";
        auto* compact = compact_raw_messages("{\"@type\":\"messages\",\"messages\":[{\"id\":1,\"chat_id\":42,\"content\":" + content + "}],\"pad\":\"" + std::string(2048, 'x') + "\"}");
        assert(compact);
        auto* message = json_array_get(json_object_get(compact, "messages"), 0);
        auto item = previews.parseMessage(message);
        assert(item.text == "https://example.org" && item.link_spans.size() == 1 && item.links.size() == 1);
        assert(item.preview_url == "https://example.org" && item.preview_site == "Example");
        assert(item.preview_title == "Article" && item.preview_description == "Описание");
        assert(item.media_file_id == 71 && item.media_path == "preview.jpg" && item.photos.empty() && !item.animation);
        UiCache cache;
        cache.rememberMessages(compact);
        auto* cached = cache.history(42);
        assert(previews.parseMessage(json_object_get(cached, "1")).preview_title == "Article");
        json_decref(cached);
        previews.history_items_.push_back(item);
        previews.messages_.push_back(item);
        auto* file = parse(R"({"id":71,"local":{"path":"new-preview.jpg","is_downloading_completed":true}})");
        previews.updateFile(file);
        json_decref(file);
        assert(previews.messages_[0].media_path == "new-preview.jpg");
        auto* body = json_object_get(message, "content");
        auto* preview = json_object_get(body, "link_preview");
        auto* video = parse(R"({"@type":"linkPreviewTypeVideo","video":{"video":{"id":99},"thumbnail":{"format":{"@type":"thumbnailFormatJpeg"},"file":{"id":72,"local":{"is_downloading_completed":false}}}}})");
        json_object_set_new(preview, "type", video);
        item = previews.parseMessage(message);
        assert(item.media_file_id == 72 && item.media_path.empty() && !item.animation && item.animation_file_id == 0);
        json_object_del(preview, "type");
        item = previews.parseMessage(message);
        assert(!item.preview_url.empty() && item.media_file_id == 0);
        json_object_set_new(body, "link_preview_options", json_pack("{s:b}", "is_disabled", 1));
        item = previews.parseMessage(message);
        assert(item.preview_url.empty() && item.media_file_id == 0 && item.text == "https://example.org");
        json_object_del(body, "link_preview_options");
        json_object_set_new(preview, "url", json_string("javascript:alert(1)"));
        assert(previews.parseMessage(message).preview_url.empty());
        json_decref(compact);
        previews.current_chat_id_ = 42;
        previews.worker_started_ = true;
        previews.process(parse(R"({"@type":"updateMessageContent","chat_id":42,"message_id":1,"new_content":{"@type":"messageText","text":{"text":"updated"}}})"));
        assert(!previews.outgoing_.empty());
        auto* request = parse(previews.outgoing_.back().text.c_str());
        assert(!std::strcmp(json_string_value(json_object_get(request, "@type")), "getMessage"));
        json_decref(request);
        previews.worker_started_ = false;
    }
    // Exercise actual outgoing TDLib requests, including captions and partial batches.
    {
        const char* path = "attachment-caption.fixture";
        FILE* fixture = std::fopen(path, "wb"); assert(fixture);
        std::fputs("attachment", fixture); std::fclose(fixture);
        TelegramClient attachments;
        attachments.ready_ = attachments.worker_started_ = true;
        attachments.current_chat_id_ = 42;
        ChatItem attachment_chat; attachment_chat.id = 42; attachment_chat.user_id = 1;
        attachments.known_chats_.push_back(attachment_chat);
        const std::string caption = "Подпись \"текст\"\nSecond line";
        auto check = [&](const std::vector<std::pair<std::string, int>>& files,
                         bool expected_ok, size_t requests) {
            attachments.outgoing_.clear();
            assert(attachments.setCompositionReply(55));
            assert(attachments.sendLocalFiles(files, 42, 7, caption) == expected_ok);
            assert(attachments.outgoing_.size() == requests);
            assert(attachments.replyTarget() == (requests ? 0 : 55));
            int captions = 0;
            for (const auto& outgoing : attachments.outgoing_) {
                auto* root = parse(outgoing.text.c_str());
                assert(MediaUploads::number(root, "chat_id") == 42);
                assert(MediaUploads::number(json_object_get(root, "reply_to"), "message_id") == 55);
                assert(MediaUploads::number(json_object_get(root, "topic_id"), "forum_topic_id") == 7);
                auto check_content = [&](json_t* content) {
                    auto* text = json_object_get(content, "caption");
                    assert(MediaUploads::string(text, "@type") == "formattedText");
                    const auto value = MediaUploads::string(text, "text");
                    if (!value.empty()) { assert(value == caption); ++captions; }
                };
                if (MediaUploads::string(root, "@type") == "sendMessageAlbum") {
                    auto* contents = json_object_get(root, "input_message_contents");
                    assert(json_array_size(contents) >= 2);
                    for (size_t i = 0; i < json_array_size(contents); ++i)
                        check_content(json_array_get(contents, i));
                } else {
                    assert(MediaUploads::string(root, "@type") == "sendMessage");
                    check_content(json_object_get(root, "input_message_content"));
                    assert(MediaUploads::string(json_object_get(root, "@extra"), "kind") == "media_upload");
                }
                json_decref(root);
            }
            assert(captions == (requests ? 1 : 0));
        };
        for (int kind = 0; kind < 3; ++kind) {
            check({{path, kind}}, true, 1);
            check({{path, kind}, {path, kind}}, true, 1);
            check({{"missing-caption.fixture", kind}, {path, kind}}, false, 1);
        }
        check({{path, 0}, {path, 1}, {path, 2}}, true, 2);
        check({{"missing-caption.fixture", 0}, {path, 2}}, false, 1);
        check({{"missing-caption.fixture", 0}, {"missing-caption.fixture", 1}}, false, 0);
        // Reply selection doesn't open the text editor and survives failed sends.
        attachments.outgoing_.clear();
        assert(attachments.setCompositionReply(55) && attachments.inputKind() == InputKind::None);
        assert(!attachments.sendLocalFile("missing-caption.fixture", 0, false, 42, 0));
        assert(attachments.replyTarget() == 55);
        assert(attachments.sendAudioFile(path, 2, 42, 7));
        auto verify_reply = [&] {
            auto* root = parse(attachments.outgoing_.back().text.c_str());
            assert(MediaUploads::number(json_object_get(root, "reply_to"), "message_id") == 55);
            assert(MediaUploads::number(json_object_get(root, "topic_id"), "forum_topic_id") == 7);
            assert(!attachments.replyTarget()); json_decref(root);
        };
        verify_reply();
        assert(attachments.setCompositionReply(55));
        SendStickerItem sticker; sticker.file = 123; attachments.stickers_.push_back(sticker);
        attachments.current_topic_ = 7;
        assert(attachments.sendSticker(0)); verify_reply();
        attachments.current_thread_ = 99; attachments.current_topic_ = 0;
        assert(attachments.setCompositionReply(55));
        attachments.requestCompose(); attachments.submitInput("Text reply");
        auto* text_reply = parse(attachments.outgoing_.back().text.c_str());
        assert(MediaUploads::number(json_object_get(text_reply, "reply_to"), "message_id") == 55);
        assert(MediaUploads::number(json_object_get(text_reply, "topic_id"), "message_thread_id") == 99);
        assert(!attachments.replyTarget()); json_decref(text_reply);
        assert(attachments.setCompositionReply(55)); attachments.cancelInput();
        assert(!attachments.replyTarget());
        assert(attachments.sendLocalFile(path, 0, false, 42, 0));
        auto* regular = parse(attachments.outgoing_.back().text.c_str());
        assert(!json_object_get(regular, "reply_to")); json_decref(regular);
        assert(attachments.setCompositionReply(55));
        assert(attachments.sendLocalFile(path, 0, false, 43, 0));
        auto* other_chat = parse(attachments.outgoing_.back().text.c_str());
        assert(!json_object_get(other_chat, "reply_to") && attachments.replyTarget() == 55); json_decref(other_chat);
        assert(attachments.setCompositionReply(0) && !attachments.replyTarget());
        attachments.worker_started_ = false;
        assert(!std::remove(path));
    }
    std::puts("Attachment captions: photo/video/document, albums, mixed batches, missing files and topic passed");
    {
        TelegramClient localized;
        localized.ready_ = localized.worker_started_ = true;
        localized.current_chat_id_ = 42;
        localized.setInput(InputKind::None, i18n::text("status.online"));
        localized.auth_detail_ = i18n::text("auth.code_sent", {"+123", i18n::text("auth.via.sms")});
        localized.proxy_check_status_ = i18n::text("proxy.check.ok", {"123 ms"});
        localized.sticker_status_ = i18n::text("sticker.choose_set");
        localized.gif_status_ = i18n::text("gif.choose");
        localized.reaction_status_ = i18n::text("reaction.hint");
        localized.topics_status_ = i18n::text("topics.none");
        localized.document_status_ = i18n::text("doc.saved", {"ux0:data/Online.pdf"});
        localized.video_note_status_ = i18n::text("video_note.send_failed");
        i18n::Text retained_send_error = localized.videoNoteStatus();
        localized.history_error_ = i18n::text("history.no_older");
        localized.error_ = "SERVER_ERROR: Online";
        localized.current_chat_title_ = i18n::Text("Online") + " / " + i18n::text("comments.title");
        localized.sticker_packs_ = {{0, i18n::text("sticker.favorites")}, {1, i18n::text("sticker.recent")}, {123, "Online"}};
        localized.folders_ = {{0, i18n::text("folder.all")}, {123, "Online"}};
        localized.poll_requests_[{42, 7}] = {false, i18n::text("poll.updated")};
        localized.user_names_[77] = ""; // Missing sender names are localized at display time.
        localized.reply_cache_[{99, 1}].text = i18n::text("media.photo");
        const std::string custom_label = tr("reaction.emoji_n", {"1"});
        localized.reaction_choices_ = {"👍", custom_label};
        localized.reaction_custom_choices_[custom_label] = 123456;
        auto* photo = parse(R"({"id":1,"chat_id":42,"content":{"@type":"messagePhoto","photo":{"sizes":[]}}})");
        localized.history_items_.push_back(localized.parseMessage(photo)); json_decref(photo);
        auto* literal = parse(R"({"id":2,"chat_id":42,"content":{"@type":"messageText","text":{"text":"Online / Все чаты"}},"reply_to":{"@type":"messageReplyToMessage","chat_id":42,"message_id":1}})");
        localized.history_items_.push_back(localized.parseMessage(literal)); json_decref(literal);
        auto* unnamed = parse(R"({"id":3,"chat_id":42,"content":{"@type":"messageDocument","document":{"file_name":""}}})");
        localized.history_items_.push_back(localized.parseMessage(unnamed)); json_decref(unnamed);
        auto* service = parse(R"({"id":4,"chat_id":42,"sender_id":{"@type":"messageSenderUser","user_id":77},"content":{"@type":"messageChatJoinByLink"}})");
        localized.history_items_.push_back(localized.parseMessage(service)); json_decref(service);
        localized.rebuildHistory(false);
        const auto requests = localized.outgoing_.size();
        const auto languages = i18n::languages();
        for (const auto& language : languages) {
            assert(i18n::select(language.code));
            localized.refreshLanguage(); // No TDLib event, reopen or restart.
            assert(localized.status() == tr("status.online"));
            assert(localized.authDetail() == tr("auth.code_sent", {"+123", tr("auth.via.sms")}));
            assert(localized.proxyCheckStatus() == tr("proxy.check.ok", {"123 ms"}));
            assert(localized.stickerStatus() == tr("sticker.choose_set"));
            assert(localized.gifStatus() == tr("gif.choose"));
            assert(localized.reactionStatus() == tr("reaction.hint"));
            assert(localized.topicsStatus() == tr("topics.none"));
            assert(localized.documentStatus() == tr("doc.saved", {"ux0:data/Online.pdf"}));
            retained_send_error.refresh();
            assert(retained_send_error == tr("video_note.send_failed"));
            assert(localized.historyError() == tr("history.no_older"));
            assert(localized.lastError() == "SERVER_ERROR: Online");
            assert(localized.currentChatTitle() == "Online / " + tr("comments.title"));
            assert(localized.folders_[0].title == tr("folder.all") && localized.folders_[1].title == "Online");
            assert(localized.folders_.back().title == tr("folder.archive"));
            assert(localized.sticker_packs_[0].title == tr("sticker.favorites"));
            assert(localized.sticker_packs_[1].title == tr("sticker.recent") && localized.sticker_packs_[2].title == "Online");
            assert(localized.poll_requests_.at({42, 7}).status == tr("poll.updated"));
            assert(localized.reply_cache_.at({99, 1}).text == tr("media.photo"));
            assert(localized.reaction_choices_[0] == "👍");
            assert(localized.reaction_choices_[1] == tr("reaction.emoji_n", {"1"}));
            assert(localized.reaction_custom_choices_.at(localized.reaction_choices_[1]) == 123456);
            assert(localized.messages_[0].media_label == tr("media.photo"));
            assert(localized.messages_[1].text == "Online / Все чаты");
            assert(localized.messages_[1].reply_text == tr("media.photo"));
            assert(localized.messages_[2].text == tr("media.no_name"));
            assert(localized.messages_[3].text == tr("service.joined_by_link", {tr("user.participant")}));
            assert(localized.outgoing_.size() == requests);
        }
        localized.worker_started_ = false;
        i18n::select("ru");
    }
    std::puts("Language refresh: all shipped locales, retained UI state, messages, replies and custom names passed without network events");
    {
        TelegramClient status_only;
        status_only.worker_started_ = true;
        for (int i = 0; i < 32; ++i)
            status_only.parsed_.push_back(parse(R"({"@type":"updateUserStatus","user_id":77,"status":{"@type":"userStatusOnline","expires":2000000000}})"));
        performance::collector().take(performance::now());
        while (!status_only.parsed_.empty()) status_only.pump(false);
        auto observations = performance::collector().take(performance::now());
        assert(observations.get(performance::Metric::PublishChats).count == 0);
        assert(observations.get(performance::Metric::UiRemember).count == 0);
        assert(status_only.user_presence_.count(77)); // State still processed.
        status_only.parsed_.push_back(parse(R"({"@type":"updateNewChat","chat":{"id":77,"title":"Visible change","positions":[{"list":{"@type":"chatListMain"},"order":100}]}})"));
        status_only.pump(false);
        assert(status_only.chats_.size() == 1 && status_only.chats_[0].title == "Visible change");
        status_only.worker_started_ = false;
        // Storage pauses while protocol queues are busy, and can be stopped.
        for (int i = 0; i < 64; ++i) status_only.incoming_.push_back({"busy", 0});
        mkdir("ux0:data",0777);mkdir("ux0:data/vitagram",0777);mkdir(VITAGRAM_FILES_DIR,0777);
        const std::string early_path=std::string(VITAGRAM_FILES_DIR)+"/early-cache.bin";
        FILE* early_file=std::fopen(early_path.c_str(),"wb");assert(early_file);std::fputs("data",early_file);std::fclose(early_file);
        auto* early=json_pack("{s:s,s:i,s:i,s:{s:s,s:b},s:{s:s}}","@type","file","id",991,"size",4,
            "local","path",early_path.c_str(),"is_downloading_completed",1,"remote","unique_id","early-cache");
        status_only.media_cache_.enqueue(early);json_decref(early);status_only.media_cache_.prioritize(991);
        std::atomic<bool> returned{false};
        bool permitted = true;
        std::thread paused([&] { permitted = TelegramClient::cacheLoadProceed(&status_only); returned = true; });
        json_t* early_ready=nullptr;
        for (int i=0;i<2000 && !early_ready;++i) { early_ready=status_only.media_cache_.takeValidated();if (!early_ready) sceKernelDelayThread(1000); }
        assert(early_ready && !returned);
        assert(json_integer_value(json_object_get(early_ready,"id"))==991);
        assert(json_is_true(json_object_get(json_object_get(early_ready,"local"),"is_downloading_completed")));
        json_decref(early_ready);
        assert(status_only.incoming_.size()==64); // Cache checks do not replay protocol events.

        status_only.stopping_ = true; paused.join(); assert(!permitted);
    }
    {
        TelegramClient preview;
        preview.ready_ = preview.worker_started_ = true;
        SendStickerItem sticker;
        sticker.file = 120; sticker.preview = 121; sticker.animated = true;
        preview.stickers_.push_back(sticker);
        preview.ensureSticker(0);
        assert(preview.pending_files_.count(121) && !preview.pending_files_.count(120));
        preview.ensureSticker(0, true);
        assert(preview.pending_files_.count(120));
        preview.ensureSticker(0, true);
        assert(preview.outgoing_.size() == 2); // held preview does not repeat requests
        preview.worker_started_ = false;
        auto* partial = parse(R"({"id":120,"local":{"path":"preview.tgs","is_downloading_completed":false}})");
        preview.updateFile(partial); json_decref(partial);
        assert(preview.stickers_[0].full_path.empty());
        auto* complete = parse(R"({"id":120,"local":{"path":"preview.tgs","is_downloading_completed":true}})");
        preview.updateFile(complete); json_decref(complete);
        assert(preview.stickers_[0].full_path == "preview.tgs" && preview.stickers_[0].path.empty());
        auto* thumbnail = parse(R"({"id":121,"local":{"path":"preview.webp","is_downloading_completed":true}})");
        preview.updateFile(thumbnail); json_decref(thumbnail);
        assert(preview.stickers_[0].path == "preview.webp" && preview.stickers_[0].full_path == "preview.tgs");
        auto* root = parse(R"({"@type":"stickers","stickers":[{"format":{"@type":"stickerFormatTgs"},"sticker":{"id":122}},{"format":{"@type":"stickerFormatWebp"},"sticker":{"id":123}}]})");
        preview.applyStickerResponse(root, false); json_decref(root);
        assert(preview.stickers_.size() == 2 && preview.stickers_[0].animated && !preview.stickers_[1].animated);
    }
    {
        TelegramClient sliced;
        auto* root=json_pack("{s:s,s:[]}","@type","stickers","stickers");
        for (int i=0;i<100;++i) json_array_append_new(json_object_get(root,"stickers"),json_pack("{s:s,s:{s:s},s:{s:s,s:i}}","emoji","x","format","@type","stickerFormatWebp","sticker","@type","file","id",1000+i));
        sliced.applyStickerResponse(root,false);
        assert(!sliced.stickers_.empty() && sliced.stickers_.size()<=8 && sliced.sticker_apply_root_);
        ++sliced.sticker_generation_;size_t before=sliced.stickers_.size();sliced.applyStickerSlice();
        assert(!sliced.sticker_apply_root_ && sliced.stickers_.size()==before);
        sliced.applyStickerResponse(root,false);
        for (int i=0;i<1000 && sliced.sticker_apply_root_;++i) sliced.applyStickerSlice();
        assert(!sliced.sticker_apply_root_ && sliced.stickers_.size()==100);
        json_object_set_new(root,"@type",json_string("stickerSet"));
        json_object_set_new(root,"id",json_integer(999));
        json_object_set_new(root,"@extra",json_pack("{s:s}","kind","sticker_pack_icon"));
        char* encoded=json_dumps(root,JSON_COMPACT);assert(encoded);
        TelegramClient icon_parser;std::string payload=encoded;std::free(encoded);
        icon_parser.incoming_.push_back({payload,sceKernelGetSystemTimeWide()});icon_parser.incoming_bytes_=payload.size();
        std::thread parser([&]{TelegramClient::parserEntry(&icon_parser);});
        json_t* compact=nullptr;
        for (int i=0;i<2000 && !compact;++i) {
            pthread_mutex_lock(&icon_parser.queue_mutex_);
            if (!icon_parser.parsed_.empty()) {compact=static_cast<json_t*>(icon_parser.parsed_.front());icon_parser.parsed_.pop_front();}
            pthread_mutex_unlock(&icon_parser.queue_mutex_);
            if (!compact) sceKernelDelayThread(1000);
        }
        icon_parser.stopping_=true;parser.join();assert(compact);
        assert(!json_object_get(compact,"stickers"));
        assert(json_integer_value(json_object_get(sticker_pack_preview(compact),"id"))==1000);
        assert(td_event_wire_bytes(compact)==payload.size());json_decref(compact);
        json_object_set_new(root,"@type",json_string("updateStickerSet"));json_object_del(root,"@extra");
        encoded=json_dumps(root,JSON_COMPACT);assert(encoded);payload=encoded;std::free(encoded);
        icon_parser.stopping_=false;icon_parser.parsed_wire_bytes_=0;
        icon_parser.incoming_.push_back({payload,sceKernelGetSystemTimeWide()});icon_parser.incoming_bytes_=payload.size();
        std::thread unused_parser([&]{TelegramClient::parserEntry(&icon_parser);});compact=nullptr;
        for (int i=0;i<2000 && !compact;++i) {
            pthread_mutex_lock(&icon_parser.queue_mutex_);
            if (!icon_parser.parsed_.empty()) {compact=static_cast<json_t*>(icon_parser.parsed_.front());icon_parser.parsed_.pop_front();}
            pthread_mutex_unlock(&icon_parser.queue_mutex_);
            if (!compact) sceKernelDelayThread(1000);
        }
        icon_parser.stopping_=true;unused_parser.join();assert(compact);
        assert(json_object_size(compact)==3 && !json_object_get(compact,"stickers"));
        assert(td_event_wire_bytes(compact)==payload.size());json_decref(compact);
        json_object_set_new(root,"@received_at",json_integer(123));json_object_set_new(root,"@wire_bytes",json_integer(456));
        auto* marker=compact_unused_td_update(root);assert(marker && !json_object_get(marker,"stickers"));
        assert(td_event_wire_bytes(marker)==456 && json_integer_value(json_object_get(marker,"@received_at"))==123);
        assert(json_object_size(marker)==3);json_decref(marker);
        json_object_set_new(root,"@extra",json_pack("{s:s}","kind","useful_reply"));assert(!compact_unused_td_update(root));
        for (const char* type:{"stickerSet","updateNewMessage","updateFile","updateMessageSendSucceeded"}) {
            json_object_set_new(root,"@type",json_string(type));json_object_del(root,"@extra");assert(!compact_unused_td_update(root));
        }
        json_decref(root);
    }
    TransientStatus notice;
    assert(notice.update("Ошибка", 1000000) == "Ошибка");
    assert(notice.update("Ошибка", 9000000).empty());
    assert(notice.update("Ошибка", 10000000).empty()); // No resurrection on redraw.
    notice.clear(); // A new action can report the same error again.
    assert(notice.update("Ошибка", 11000000) == "Ошибка");
    assert(notice.update("", 12000000).empty());

    MediaUploads u;
    json_decref(u.begin("video.mp4",10000,true,1,1000000));
    json_decref(u.begin("audio.ogg",2000,false,2,1000000));
    event(u,R"({"@type":"updateFile","file":{"id":10,"size":10000,"local":{"path":"video.mp4"},"remote":{"uploaded_size":2500,"is_uploading_active":true}}})",2000000);
    assert(u.items()[0].done==2500 && u.items()[0].speed==0); // First observation isn't a speed sample.
    assert(u.items()[1].done==0);
    assert(u.summary(2000000, 1).find("25%")!=std::string::npos);
    assert(u.summary(9000000, 1).find("нет прогресса 7 с")!=std::string::npos);
    event(u,R"({"@type":"message","chat_id":1,"id":101,"sending_state":{"@type":"messageSendingStatePending"},"@extra":{"kind":"media_upload","token":1},"content":{"video_note":{"video":{"id":10,"size":10000,"remote":{"uploaded_size":2500,"is_uploading_active":true}}}}})",3000000);
    event(u,R"({"@type":"message","chat_id":2,"id":101,"sending_state":{"@type":"messageSendingStatePending"},"@extra":{"kind":"media_upload","token":2},"content":{"voice_note":{"voice":{"id":11,"size":2000,"remote":{"uploaded_size":2000,"is_uploading_completed":true}}}}})",3000000);
    assert(!u.items()[1].terminal && u.items()[1].uploaded);
    event(u,R"({"@type":"updateMessageSendSucceeded","old_message_id":101,"message":{"chat_id":2,"id":900}})",4000000);
    assert(!u.items()[0].terminal && u.items()[1].terminal);
    assert(u.summary(4000000, 2).find("отправлено") != std::string::npos);
    assert(u.summary(12000000, 2).empty());
    assert(u.summary(4000000, 3).empty());
    u.dismissCompleted();
    assert(u.summary(4000000, 2).empty());
    assert(!u.summary(4000000, 1).empty()); // Ongoing uploads survive new actions.
    assert(!u.lines(4000000, true).empty()); // Diagnostic history is preserved.
    event(u,R"({"@type":"updateFile","file":{"id":10,"remote":{"uploaded_size":0,"is_uploading_active":true}}})",4000000);
    assert(u.items()[0].speed==0); // Retry does not produce a negative ETA.
    event(u,R"({"@type":"updateMessageSendFailed","old_message_id":101,"message":{"chat_id":1},"error":{"message":"NETWORK_ERROR"}})",5000000);
    assert(u.items()[0].terminal && u.items()[0].error=="NETWORK_ERROR");
    assert(u.summary(5000000, 1).find("NETWORK_ERROR") != std::string::npos);
    assert(u.summary(5000000, 2).empty()); // Old completed results stay dismissed.
    u.dismissCompleted();
    assert(u.summary(5000000, 1).empty());
    assert(u.lines(21000000).empty());
    json_decref(u.begin("other.mp4",1000,true,3,22000000));
    event(u,R"({"@type":"error","@extra":{"kind":"media_upload","token":3},"message":"FILE_ERROR"})",23000000);
    assert(u.items().back().error=="FILE_ERROR");

    // Opening the UI must subscribe TDLib to channel/supergroup updates.
    // Media uploads and their results must not close that subscription.
    {
        TelegramClient live;
        live.ready_ = true; live.worker_started_ = true; // Inspect requests without a TD thread.
        live.openChat(42, "Live chat");
        auto request_is = [&](size_t index, const char* method, int64_t chat) {
            auto* request = parse(live.outgoing_[index].text.c_str());
            assert(MediaUploads::string(request, "@type") == method);
            assert(MediaUploads::number(request, "chat_id") == chat);
            json_decref(request);
        };
        request_is(0, "openChat", 42);
        request_is(1, "getChatHistory", 42);
        assert(live.outgoing_[0].priority > live.outgoing_[1].priority);
        live.outgoing_.clear();
        live.syncOpenedChat();
        assert(live.outgoing_.empty());
        auto* pending = live.uploads_.begin("voice.ogg", 2000, false, 42, 1000000);
        json_decref(pending);
        live.process(parse(R"({"@type":"error","@extra":{"kind":"media_upload","token":1},"message":"CANCELLED"})"));
        live.parsed_.push_back(parse(R"({"@type":"updateNewMessage","message":{"id":501,"chat_id":42,"content":{"@type":"messageText","text":{"text":"После записи"}}}})"));
        live.pump();
        assert(live.messages().size() == 1 && live.messages()[0].id == 501);
        assert(live.opened_chat_id_ == 42);
        live.outgoing_.clear();
        live.openChat(43, "Next chat");
        request_is(0, "closeChat", 42);
        request_is(1, "openChat", 43);
        live.outgoing_.clear();
        live.leaveChat();
        request_is(0, "closeChat", 43);
        assert(live.next_update_check_ == 0);
        live.outgoing_.clear();
        live.leaveChat();
        assert(live.outgoing_.empty());
        live.notice_ = "Old result"; live.notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
        live.dismissStatusNotices();
        assert(live.updateNotice().empty());
        ChatItem known; known.id = 43; known.title = "Next chat";
        live.known_chats_.push_back(known);
        live.refreshChats();
        auto* refresh = parse(live.outgoing_[0].text.c_str());
        auto* reply = json_pack("{s:s,s:[I]}", "@type", "chats", "chat_ids", (json_int_t)43);
        json_object_set(reply, "@extra", json_object_get(refresh, "@extra"));
        json_decref(refresh); live.outgoing_.clear();
        live.process(reply);
        request_is(0, "getChat", 43); // Explicit refresh also updates known metadata.
        live.outgoing_.clear();
        live.ready_ = false;
        live.openChat(44, "Cached chat");
        assert(live.outgoing_.empty());
        live.process(parse(R"({"@type":"updateAuthorizationState","authorization_state":{"@type":"authorizationStateReady"}})"));
        auto* identity = parse(live.outgoing_[0].text.c_str());
        assert(MediaUploads::string(identity, "@type") == "getMe");
        assert(MediaUploads::string(identity, "@extra") == "account_identity"); json_decref(identity);
        request_is(1, "openChat", 44);
        assert(std::any_of(live.outgoing_.begin(), live.outgoing_.end(), [](const auto& request) {
            return request.text.find("\"ignore_file_names\"") != std::string::npos; })); // One create per download.
        live.worker_started_ = false;
    }

    // Navigation invalidates link/history replies, including leave-and-reopen
    // of the same chat and switching to another topic in the same forum.
    {
        TelegramClient nav;
        nav.ready_ = nav.worker_started_ = true;
        nav.openChat(42, "Source");
        auto link_reply = [&]() {
            nav.outgoing_.clear();
            assert(nav.openTelegramLink("https://t.me/example"));
            auto* request = parse(nav.outgoing_.back().text.c_str());
            auto* reply = parse(R"({"@type":"chat","id":99,"title":"Linked chat"})");
            json_object_set(reply, "@extra", json_object_get(request, "@extra"));
            json_decref(request);
            return reply;
        };
        auto* old_link = link_reply();
        nav.openChat(43, "Other"); nav.process(old_link);
        assert(nav.currentChatId() == 43 && !nav.link_chat_opened_);
        old_link = link_reply();
        nav.leaveChat(); nav.openChat(43, "Other again"); nav.process(old_link);
        assert(nav.currentChatId() == 43 && !nav.link_chat_opened_);
        old_link = link_reply();
        nav.topics_.push_back({}); nav.topics_[0].id = 7;
        nav.openTopic(0); nav.process(old_link);
        assert(nav.currentChatId() == 43 && nav.currentTopicId() == 7 && !nav.link_chat_opened_);
        auto* history = parse(nav.outgoing_.back().text.c_str());
        auto* late = parse(R"({"@type":"messages","messages":[{"id":55,"chat_id":43,"content":{"@type":"messageText","text":{"text":"Old topic"}}}]})");
        json_object_set(late, "@extra", json_object_get(history, "@extra")); json_decref(history);
        nav.backToTopics(); nav.process(late);
        assert(nav.messages().empty() && !nav.history_pending_);
        nav.openTopic(0);
        nav.process(link_reply());
        assert(nav.currentChatId() == 99 && nav.link_chat_opened_);
        nav.worker_started_ = false;
    }
    std::puts("Navigation: stale links/history ignored after chat/topic changes; current link opens");

    // Explicit document exports continue outside their original window and
    // failed attempts are retryable; an old failure cannot cancel the retry.
    {
        TelegramClient docs;
        docs.ready_ = docs.worker_started_ = true;
        docs.openChat(42, "Files"); docs.outgoing_.clear();
        auto* payload = parse(R"({"id":10,"chat_id":42,"content":{"@type":"messageDocument","document":{"file_name":"report.pdf","document":{"id":701,"size":2097152,"local":{"downloaded_size":0}}}}})");
        auto item = docs.parseMessage(payload); json_decref(payload);
        assert(item.document && item.document_size == 2097152);
        docs.downloadDocument(item, "ux0:data/downloads");
        auto* request = parse(docs.outgoing_.back().text.c_str());
        auto error_reply = [&]() {
            auto* reply = parse(R"({"@type":"error","message":"NETWORK_ERROR"})");
            json_object_set(reply, "@extra", json_object_get(request, "@extra")); return reply;
        };
        docs.openChat(43, "Other");
        docs.process(parse(R"({"@type":"updateFile","file":{"id":701,"size":2097152,"local":{"downloaded_size":1048576}}})"));
        assert(docs.documentStatus() == tr("doc.downloading", {"50%"}));
        docs.process(error_reply());
        assert(docs.document_downloads_.empty() && !docs.pending_files_.count(701));
        assert(docs.documentStatus() == tr("doc.download_failed", {"NETWORK_ERROR"}));
        docs.downloadDocument(item, "ux0:data/downloads");
        assert(docs.document_downloads_.count(701));
        docs.process(error_reply());
        assert(docs.document_downloads_.count(701) && docs.pending_files_.count(701));
        json_decref(request);
        docs.messages_ = docs.history_items_ = {item};
        docs.process(parse(R"({"@type":"updateFile","file":{"id":701,"size":3145728,"local":{"downloaded_size":1048576}}})"));
        assert(docs.messages_[0].document_size == 3145728 && docs.history_items_[0].document_size == 3145728);
        assert(docs.takeSavedDocument().empty()); // Errors never offer installation.
        auto spit = [](const char* path, const char* text) {
            FILE* f = std::fopen(path, "wb"); assert(f);
            assert(std::fwrite(text, 1, std::strlen(text), f) == std::strlen(text)); assert(std::fclose(f) == 0);
        };
        auto slurp = [](const std::string& path) {
            FILE* f = std::fopen(path.c_str(), "rb"); assert(f);
            char bytes[64]{}; size_t n = std::fread(bytes, 1, sizeof(bytes), f); std::fclose(f);
            return std::string(bytes, n);
        };
        auto saved_export = [&]() {
            const auto deadline = sceKernelGetSystemTimeWide() + 3000000;
            while (!docs.hasSavedDocuments() && sceKernelGetSystemTimeWide() < deadline) {
                docs.applyDocumentExports(); sceKernelDelayThread(1000);
            }
            return docs.takeSavedDocument();
        };
        spit("ux0:data/export-source.vpk", "package bytes");
        export_blocked = true; export_entered = false; export_released = false;
        docs.process(parse(R"({"@type":"updateFile","file":{"id":701,"size":13,"local":{"path":"ux0:data/export-source.vpk","is_downloading_completed":true,"downloaded_size":13}}})"));
        const auto deadline = sceKernelGetSystemTimeWide() + 3000000;
        while (!export_entered && sceKernelGetSystemTimeWide() < deadline) sceKernelDelayThread(1000);
        assert(export_entered && docs.document_downloads_.at(701).exporting);
        // A blocked storage device must not block event handling or the UI pump.
        const auto before = sceKernelGetSystemTimeWide();
        docs.pump(true);
        docs.process(parse(R"({"@type":"updateFile","file":{"id":701,"local":{"path":"ux0:data/export-source.vpk","is_downloading_completed":true}}})"));
        assert(sceKernelGetSystemTimeWide() - before < 100000);
        assert(docs.document_exports_.empty()); // Repeated completion does not queue another export.
        assert(docs.takeSavedDocument().empty());
        export_released = true; export_blocked = false;
        std::string exported = saved_export();
        assert(!exported.empty() && docs.takeSavedDocument().empty());
        assert(slurp(exported) == "package bytes");
        item.media_path = "ux0:data/export-source.vpk"; item.document_name = "cached.VPK";
        docs.downloadDocument(item, "ux0:data/downloads");
        exported = saved_export();
        assert(!exported.empty() && slurp(exported) == "package bytes");
        // Saving again compares the identical export on Storage, not UI.
        export_blocked = true; export_entered = false; export_released = false;
        docs.downloadDocument(item, "ux0:data/downloads");
        const auto compare_deadline = sceKernelGetSystemTimeWide() + 3000000;
        while (!export_entered && sceKernelGetSystemTimeWide() < compare_deadline) sceKernelDelayThread(1000);
        assert(export_entered && docs.document_downloads_.at(701).exporting);
        docs.pump(true);
        assert(docs.takeSavedDocument().empty());
        export_released = true; export_blocked = false;
        assert(saved_export() == exported);
        assert(docs.documentStatus() == tr("doc.exists", {exported}));
        struct stat copy{};
        const std::string duplicate = exported.substr(0, exported.size() - 4) + " (1).VPK";
        assert(stat(duplicate.c_str(), &copy) != 0);
        // Without a cached copy the export in the folder blocks a second download.
        const size_t sent = docs.outgoing_.size();
        item.media_path.clear(); item.document_size = 13; item.media_file_id = 702;
        docs.downloadDocument(item, "ux0:data/downloads");
        assert(docs.outgoing_.size() == sent && !docs.document_downloads_.count(702));
        assert(docs.takeSavedDocument() == exported && docs.documentStatus() == tr("doc.exists", {exported}));
        // A different file under the same name is still saved beside it.
        spit("ux0:data/export-other.vpk", "other package");
        item.media_path = "ux0:data/export-other.vpk";
        docs.downloadDocument(item, "ux0:data/downloads");
        const std::string other = saved_export();
        assert(other == duplicate && slurp(other) == "other package");
        docs.downloadDocument(item, "ux0:data/downloads");
        assert(saved_export() == other);
        docs.downloadDocument(item, "ux0:data/downloads");
        docs.clearAccountMemory();
        assert(docs.takeSavedDocument().empty());
        docs.worker_started_ = false;
        const std::string big(150000, 'x');
        spit("ux0:data/export-large.bin", big.c_str());
        std::string partial;
        int64_t copied = 0;
        assert(!save_downloaded_file("ux0:data/export-large.bin", "ux0:data/downloads", "partial.bin", 800,
            partial, nullptr, [&] { return copied < 65536; },
            [&](int64_t done, int64_t total) { assert(done > copied && total == 150000); copied = done; }));
        assert(copied == 65536 && partial.empty() && stat("ux0:data/downloads/partial.bin", &copy) != 0);
        assert(stat("ux0:data/downloads/.vitagram-800.part", &copy) != 0);
        copied = 0;
        assert(save_downloaded_file("ux0:data/export-large.bin", "ux0:data/downloads", "complete.bin", 800,
            partial, nullptr, {},
            [&](int64_t done, int64_t total) { assert(done > copied && total == 150000); copied = done;
                assert(stat("ux0:data/downloads/complete.bin", &copy) != 0); }));
        assert(copied == 150000 && stat(partial.c_str(), &copy) == 0 && copy.st_size == 150000);
        int calls = 0;
        assert(!save_downloaded_file("ux0:data/export-source.vpk", "ux0:data/downloads", "cancelled.vpk", 800,
            partial, nullptr, [&] { return ++calls < 4; }));
        assert(partial.empty() && stat("ux0:data/downloads/cancelled.vpk", &copy) != 0);
        // A cancelled duplicate comparison must not fall through to writing another copy.
        calls = 0;
        assert(!save_downloaded_file("ux0:data/export-source.vpk", "ux0:data/downloads", "cached.VPK", 800,
            partial, nullptr, [&] { return ++calls < 3; }));
        assert(partial.empty() && stat("ux0:data/downloads/cached (2).VPK", &copy) != 0);
        assert(!save_downloaded_file("missing-source", "ux0:data/downloads", "missing.vpk", 800, partial));
    }
    std::puts("Documents: size refresh, progress after navigation, download errors, stale retry failures and repeated exports passed");

    // Opening a chat drops chat-list avatar downloads: unsent ones leave the
    // queue, sent ones are cancelled if TDLib has not started them.
    {
        TelegramClient list;
        list.ready_ = true; list.worker_started_ = true;
        list.requestFile(501, 8, true); list.requestFile(502, 8, true); list.requestFile(503, 8);
        list.outgoing_.erase(list.outgoing_.begin() + 1); // 502 already went to TDLib.
        list.openChat(60, "Chat");
        size_t cancels = 0;
        for (const auto& request : list.outgoing_) {
            auto* root = parse(request.text.c_str());
            const auto method = MediaUploads::string(root, "@type");
            const auto file = MediaUploads::number(root, "file_id");
            assert(!(method == "downloadFile" && file == 501));
            if (method == "cancelDownloadFile") { assert(file == 502); ++cancels; }
            json_decref(root);
        }
        assert(cancels == 1 && !list.pending_files_.count(501) && !list.pending_files_.count(502));
        assert(list.pending_files_.count(503)); // Sender/media downloads are not list avatars.
        list.worker_started_ = false;
    }

    // Groups: the first history page must not wait for getSupergroup. A known
    // ordinary group skips the check; a known forum goes straight to topics.
    {
        TelegramClient groups;
        groups.ready_ = true; groups.worker_started_ = true;
        for (int64_t id : {50, 51, 52, 53}) {
            ChatItem chat; chat.id = id; chat.group = true; chat.supergroup_id = id - 40;
            groups.known_chats_.push_back(chat);
        }
        auto sent = [&](const char* method, const char* kind = nullptr) {
            json_t* found = nullptr;
            for (const auto& request : groups.outgoing_) {
                auto* root = parse(request.text.c_str());
                if (!found && MediaUploads::string(root, "@type") == method &&
                    (!kind || MediaUploads::string(json_object_get(root, "@extra"), "kind") == kind)) found = root;
                else json_decref(root);
            }
            return found;
        };
        auto answer_forum_check = [&](bool forum) {
            auto* request = sent("getSupergroup", "forum_check"); assert(request);
            auto* reply = json_pack("{s:s,s:I,s:b}", "@type", "supergroup", "id",
                (json_int_t)MediaUploads::number(request, "supergroup_id"), "is_forum", forum);
            json_object_set(reply, "@extra", json_object_get(request, "@extra"));
            json_decref(request); groups.outgoing_.clear();
            groups.process(reply);
        };
        groups.openChat(50, "Unknown group");
        auto* history = sent("getChatHistory"); assert(history); json_decref(history);
        assert(!groups.topicsOpen() && groups.history_pending_);
        answer_forum_check(false);
        history = sent("getChatHistory"); assert(!history); // The in-flight page is kept.
        assert(!groups.topicsOpen() && groups.history_pending_);

        groups.outgoing_.clear();
        groups.openChat(51, "Unknown forum");
        history = sent("getChatHistory"); assert(history);
        auto* stale = json_pack("{s:s,s:[{s:I,s:I,s:{s:s,s:{s:s}}}]}", "@type", "messages", "messages",
            "id", (json_int_t)900, "chat_id", (json_int_t)51, "content", "@type", "messageText", "text", "text", "x");
        json_object_set(stale, "@extra", json_object_get(history, "@extra")); json_decref(history);
        answer_forum_check(true);
        assert(groups.topicsOpen());
        auto* topics = sent("getForumTopics"); assert(topics); json_decref(topics);
        groups.process(stale); // Started before the forum answer: rejected by generation.
        assert(groups.messages().empty());

        groups.process(parse(R"({"@type":"updateSupergroup","supergroup":{"@type":"supergroup","id":12,"is_forum":false}})"));
        groups.process(parse(R"({"@type":"updateSupergroup","supergroup":{"@type":"supergroup","id":13,"is_forum":true}})"));
        groups.outgoing_.clear();
        groups.openChat(52, "Known group");
        assert(!sent("getSupergroup") && !groups.topicsOpen());
        history = sent("getChatHistory"); assert(history); json_decref(history);
        groups.outgoing_.clear();
        groups.openChat(53, "Known forum");
        assert(!sent("getSupergroup") && !sent("getChatHistory") && groups.topicsOpen());
        topics = sent("getForumTopics"); assert(topics); json_decref(topics);
        groups.worker_started_ = false;
    }
    std::puts("Status: expiry, dismissal, chat scope and live chat subscription passed");

    {
        TelegramClient list;
        list.ready_ = true; list.worker_started_ = true;
        ChatItem visible; visible.id = 7; visible.title = "Still visible";
        list.chats_.push_back(visible);
        list.requestChatsPage(); list.refreshChats();
        assert(list.chats_pending_ && list.refresh_pending_);
        unsigned old_generation = list.list_generation_;
        list.recoverListRequests(std::min(list.chats_request_deadline_, list.refresh_deadline_) - 1);
        assert(list.refresh_pending_);
        list.recoverListRequests(list.refresh_deadline_ + 1);
        assert(!list.chats_pending_ && !list.refresh_pending_ && list.refresh_ids_.empty());
        assert(list.list_generation_ == old_generation + 1 && list.chats_.size() == 1);
        list.outgoing_.clear();
        list.requestChatsPage(); list.refreshChats();
        auto* stale = json_pack("{s:s,s:{s:s,s:I}}", "@type", "ok", "@extra", "kind", "load_main_chats", "list_generation", (json_int_t)old_generation);
        list.process(stale);
        assert(list.chats_pending_ && list.refresh_pending_);
        list.process(parse(R"({"@type":"error","message":"late","@extra":{"kind":"refresh_main_ids","list_generation":0}})"));
        assert(list.refresh_pending_ && list.chats_error_.empty());
        auto* old_refresh = json_pack("{s:s,s:s,s:{s:s,s:I,s:I}}", "@type", "error", "message", "old refresh",
            "@extra", "kind", "refresh_main_ids", "list_generation", (json_int_t)list.list_generation_,
            "refresh_generation", (json_int_t)(list.refresh_generation_ - 1));
        list.process(old_refresh);
        assert(list.refresh_pending_ && list.chats_error_.empty());
        auto* stale_chat = json_pack("{s:s,s:I,s:s,s:{s:s,s:I}}", "@type", "chat", "id", (json_int_t)7,
            "title", "Stale title", "@extra", "kind", "refresh_main_chat", "list_generation", (json_int_t)old_generation);
        list.process(stale_chat);
        assert(list.known_chats_.empty() && list.chats_[0].title == "Still visible");
        list.process(parse(R"({"@type":"updateConnectionState","state":{"@type":"connectionStateReady"}})"));
        assert(list.refresh_pending_ && list.chats_pending_); // Fresh requests survive reconnection.
        unsigned connected_generation = list.list_generation_;
        list.process(parse(R"({"@type":"updateConnectionState","state":{"@type":"connectionStateReady"}})"));
        assert(list.list_generation_ == connected_generation);
        list.outgoing_.clear(); list.pump();
        assert(list.refresh_pending_ && list.chats_pending_ && list.outgoing_.empty());
        const int64_t stuck_now = sceKernelGetSystemTimeWide();
        list.chats_request_deadline_ = stuck_now + 5000000; list.refresh_deadline_ = stuck_now + 5000000;
        unsigned stuck_generation = list.list_generation_;
        list.resumeListRequests(stuck_now);
        assert(!list.chats_pending_ && !list.refresh_pending_ && list.list_generation_ == stuck_generation + 1);
        list.worker_started_ = false;
    }
    {
        TelegramClient list;
        list.process(parse(R"({"@type":"updateNewChat","chat":{"id":1,"title":"A","positions":[{"list":{"@type":"chatListMain"},"order":100}]}})"));
        list.process(parse(R"({"@type":"updateNewChat","chat":{"id":2,"title":"B","positions":[{"list":{"@type":"chatListMain"},"order":200}]}})"));
        list.publishChats(); assert(list.chats_[0].id == 2);
        list.process(parse(R"({"@type":"updateChatDraftMessage","chat_id":1,"positions":[{"list":{"@type":"chatListMain"},"order":300}]})"));
        list.publishChats(); assert(list.chats_[0].id == 1);
    }
    std::puts("Chat updates: request timeout, late replies, reconnect recovery and draft order passed");
    {
        TelegramClient proxy;
        proxy.worker_started_ = true; proxy.tdlib_parameters_sent_ = true;
        proxy.checkProxy("test.example", 443, "test-secret");
        assert(proxy.outgoing_.size() == 1 && proxy.proxy_check_deadline_);
        auto* request = parse(proxy.outgoing_[0].text.c_str());
        assert(MediaUploads::string(request, "@type") == "pingProxy");
        auto* value = json_object_get(request, "proxy");
        assert(MediaUploads::string(value, "server") == "test.example");
        assert(MediaUploads::number(value, "port") == 443);
        assert(MediaUploads::string(json_object_get(value, "type"), "secret") == "test-secret");
        assert(!proxy.proxy_enabled_ && proxy.proxy_.server.empty()); // Draft check never applies settings.
        proxy.checkProxy("test.example", 443, "test-secret");
        assert(proxy.outgoing_.size() == 1);
        auto* result = json_pack("{s:s,s:f}", "@type", "seconds", "seconds", 0.123);
        json_object_set(result, "@extra", json_object_get(request, "@extra"));
        proxy.process(result);
        assert(proxy.proxyCheckStatus().find("123 мс") != std::string::npos);
        proxy.cancelProxyCheck();
        auto* late = json_pack("{s:s,s:f}", "@type", "seconds", "seconds", 0.999);
        json_object_set(late, "@extra", json_object_get(request, "@extra"));
        proxy.process(late); assert(proxy.proxyCheckStatus().empty());
        json_decref(request);
        proxy.outgoing_.clear(); proxy.checkProxy("test.example", 443, "test-secret");
        request = parse(proxy.outgoing_[0].text.c_str());
        proxy.proxy_check_deadline_ = sceKernelGetSystemTimeWide() - 1; proxy.pump();
        assert(!proxy.proxy_check_deadline_ && proxy.proxyCheckStatus().find("15 секунд") != std::string::npos);
        late = json_pack("{s:s,s:f}", "@type", "seconds", "seconds", 0.999);
        json_object_set(late, "@extra", json_object_get(request, "@extra"));
        proxy.process(late); assert(proxy.proxyCheckStatus().find("15 секунд") != std::string::npos);
        json_decref(request);
        proxy.outgoing_.clear(); proxy.checkProxy("test.example", 443, "test-secret");
        request = parse(proxy.outgoing_[0].text.c_str());
        auto* failure = json_pack("{s:s,s:s}", "@type", "error", "message", "test-secret");
        json_object_set(failure, "@extra", json_object_get(request, "@extra"));
        proxy.process(failure); json_decref(request);
        assert(!proxy.proxy_check_deadline_ && proxy.proxyCheckStatus().find("недоступен") != std::string::npos);
        assert(proxy.proxyCheckStatus().find("test-secret") == std::string::npos);
        proxy.outgoing_.clear(); proxy.checkProxy("test.example", 0, "test-secret");
        assert(proxy.outgoing_.empty() && !proxy.proxy_check_deadline_);
        proxy.worker_started_ = false;
    }
    std::puts("MTProxy: draft ping, latency, duplicate prevention, cancellation, timeout and validation passed");

    {
        TelegramClient diagnostic;
        const auto received=performance::now();
        auto* root=parse(R"({"@type":"messages","messages":[],"@extra":{"kind":"history","chat":42,"generation":1,"first":true}})");
        json_object_set_new(json_object_get(root,"@extra"),"send_at",json_integer(received-12345));
        char* wire=json_dumps(root,JSON_COMPACT);json_decref(root);
        diagnostic.incoming_.push_back({wire,received});diagnostic.incoming_bytes_=std::strlen(wire);std::free(wire);
        root=parse(R"({"@type":"error","code":500,"message":"test","@extra":{"kind":"forum_check","chat":42,"generation":2}})");
        json_object_set_new(json_object_get(root,"@extra"),"send_at",json_integer(received-23456));
        wire=json_dumps(root,JSON_COMPACT);json_decref(root);
        diagnostic.incoming_.push_back({wire,received});diagnostic.incoming_bytes_+=std::strlen(wire);std::free(wire);
        performance::collector().take(performance::now());
        std::thread parser([&]{TelegramClient::parserEntry(&diagnostic);});
        bool done=false;
        for (int i=0;i<2000 && !done;++i) {
            pthread_mutex_lock(&diagnostic.queue_mutex_);done=diagnostic.parsed_.size()==2;pthread_mutex_unlock(&diagnostic.queue_mutex_);
            if (!done) sceKernelDelayThread(1000);
        }
        diagnostic.stopping_=true;parser.join();assert(done);
        auto measured=performance::collector().take(performance::now());
        assert(measured.get(performance::Metric::FirstHistoryResponse).count==1);
        assert(measured.get(performance::Metric::FirstHistoryResponse).maximum==12345);
        assert(measured.get(performance::Metric::ForumResponse).count==1);
        assert(measured.get(performance::Metric::ForumResponse).maximum==23456);
        assert(measured.get(performance::Metric::FirstHistoryParse).count==1);
        for (auto* queued:diagnostic.parsed_) json_decref(static_cast<json_t*>(queued));diagnostic.parsed_.clear();
    }
    // Run the production TD worker/parser while UI consumption is deliberately stopped.
    // The old 128+128 caps stop receive() long before all these updates are delivered.
    TelegramClient tg;
    tg.worker_started_=true; tg.parser_started_=true;
    tg.sendJson(parse(R"({"@type":"getChatHistory","chat_id":42,"limit":5,"@extra":{"kind":"history","first":true}})"));
    tg.sendJson(parse(R"({"@type":"getSupergroup","supergroup_id":1,"@extra":{"kind":"forum_check"}})"));
    assert(!pthread_create(&tg.worker_,nullptr,TelegramClient::workerEntry,&tg));
    assert(!pthread_create(&tg.parser_,nullptr,TelegramClient::parserEntry,&tg));
    auto deadline=sceKernelGetSystemTimeWide()+5000000;
    while (produced<6000 && sceKernelGetSystemTimeWide()<deadline) sceKernelDelayThread(1000);
    assert(produced==6000);
    assert(tracked_sends>=2);
    for (;;) {
        pthread_mutex_lock(&tg.queue_mutex_);
        bool empty=tg.incoming_.empty();
        size_t count=tg.parsed_.size();
        size_t raw_bytes = 0, parsed_bytes = 0;
        for (const auto& item : tg.incoming_) raw_bytes += item.text.size();
        for (auto* item : tg.parsed_) parsed_bytes += td_event_wire_bytes(static_cast<json_t*>(item));
        assert(raw_bytes == tg.incoming_bytes_ && parsed_bytes == tg.parsed_wire_bytes_);
        pthread_mutex_unlock(&tg.queue_mutex_);
        if (empty && count==601) break;
        assert(sceKernelGetSystemTimeWide()<deadline); sceKernelDelayThread(1000);
    }
    int before=receive_count; sceKernelDelayThread(10000); assert(receive_count>before);
    // Navigation bypasses congestion; ordinary requests wait for the UI to drain.
    const int sent_before=sent;
    tg.prioritizeInteraction(sceKernelGetSystemTimeWide());
    tg.sendRaw("{\"@type\":\"openChat\",\"chat_id\":1}", 4);
    while (sent==sent_before && sceKernelGetSystemTimeWide()<deadline) sceKernelDelayThread(1000);
    assert(sent==sent_before+1);
    tg.sendRaw("{\"@type\":\"getChat\",\"chat_id\":1}", 1);
    sceKernelDelayThread(10000); assert(sent==sent_before+1);
    tg.interaction_until_ = 0;
    // The first history page is still unanswered: background stays queued.
    sceKernelDelayThread(10000); assert(sent==sent_before+1);
    tg.foreground_until_ = 0; // As if its reply arrived.
    while (sent==sent_before+1 && sceKernelGetSystemTimeWide()<deadline) { tg.pump(); sceKernelDelayThread(1000); }
    assert(sent>sent_before+1);
    while (tg.user_presence_.size()<600 && sceKernelGetSystemTimeWide()<deadline) tg.pump();
    assert(tg.user_presence_.size()==600); // No control updates lost during progress coalescing.
    tg.stopWorker();
    assert(!tg.incoming_bytes_ && !tg.parsed_wire_bytes_);
    auto measured = performance::collector().take(performance::now());
    assert(measured.get(performance::Metric::Parse).count > 0);
    assert(measured.get(performance::Metric::Receive).count > 0);
    assert(measured.get(performance::Metric::EventWait).count > 0);
    assert(measured.gauges[static_cast<size_t>(performance::Gauge::IncomingBytes)].current == 0);
    assert(measured.gauges[static_cast<size_t>(performance::Gauge::ParsedWireBytes)].current == 0);

    {
        TelegramClient metrics;
        json_decref(metrics.uploads_.begin("metrics.ogg", 5000, false, 42, 1000000));
        metrics.process(parse(R"({"@type":"updateFile","@received_at":2000000,"file":{"id":8,"size":5000,"local":{"path":"metrics.ogg"},"remote":{"uploaded_size":1000,"is_uploading_active":true}}})"));
        metrics.process(parse(R"({"@type":"updateFile","@received_at":3000000,"file":{"id":8,"remote":{"uploaded_size":3000,"is_uploading_active":true}}})"));
        assert(metrics.uploads_.items()[0].speed == 2000); // UI wall time isn't transfer time.
        assert(metrics.uploads_pending_); // Disk cache yields until a terminal result.
        MessageItem voice; voice.voice_file_id = 8; voice.voice_path = "kept.ogg";
        metrics.messages_.push_back(voice); metrics.history_items_.push_back(voice);
        metrics.process(parse(R"({"@type":"updateFile","@received_at":2500000,"file":{"id":8,"local":{"is_downloading_completed":false},"remote":{"uploaded_size":0}}})"));
        assert(metrics.messages_[0].voice_path == "kept.ogg");
        assert(metrics.uploads_.items()[0].done == 3000);
        metrics.worker_started_ = true;
        enqueue_td_event(metrics.parsed_, parse(R"({"@type":"updateUserStatus","user_id":99,"status":{"@type":"userStatusOnline"}})"));
        metrics.pump(true);
        assert(metrics.parsed_.size() == 1 && metrics.user_presence_.empty());
        for (int frame = 0; frame < 4 && !metrics.parsed_.empty(); ++frame) metrics.pump(true);
        assert(metrics.parsed_.empty() && metrics.user_presence_.size() == 1); // Held touch cannot starve updates.
        metrics.pump();
        assert(metrics.parsed_.empty() && metrics.user_presence_.size() == 1);
        metrics.worker_started_ = false;
        metrics.process(parse(R"({"@type":"error","code":400,"message":"mock send failed","@extra":{"kind":"media_upload","token":1}})"));
        assert(!metrics.uploads_pending_);
    }

    // The production startup can run transport/parser before restoring the UI
    // cache, without starting a concurrent cache writer. TDLib is still mocked.
    {
        TelegramClient startup;
        assert(startup.start(true));
        assert(startup.worker_started_ && startup.parser_started_ && !startup.cache_worker_started_);
        startup.restoreCache();
        assert(startup.startCacheWorker());
        assert(startup.cache_worker_started_);
        assert(startup.startCacheWorker()); // No second writer.
        startup.stopWorker();
    }

    // Production request builders preserve original chat/topic and unique correlation.
    {
        mkdir("ux0:data", 0777); mkdir("ux0:data/vitagram", 0777);
        mkdir(VITAGRAM_DB_DIR, 0777); mkdir(VITAGRAM_FILES_DIR, 0777);
        FILE* media = std::fopen(slow_media_path, "wb"); assert(media);
        std::fputs("data", media); std::fclose(media);
        TelegramClient startup;
        assert(startup.start(true)); assert(startup.startCacheWorker());
        storage_blocked = true;
        startup.process(parse(R"({"@type":"updateUser","user":{"id":90001,"first_name":"Before","profile_photo":{"small":{"@type":"file","id":991,"size":4,"remote":{"unique_id":"slow-startup"},"local":{"path":"ux0:data/vitagram/files/slow-startup.bin","is_downloading_completed":true}}}}})"));
        auto until = sceKernelGetSystemTimeWide() + 3000000;
        while (!storage_entered && sceKernelGetSystemTimeWide() < until) sceKernelDelayThread(1000);
        assert(storage_entered);
        startup.current_chat_id_ = 90002;
        // Real parser must deliver following updates while stat is still blocked.
        pthread_mutex_lock(&startup.queue_mutex_);
        for (int i = 0; i < 1000; ++i) {
            std::string wire = i == 0 ?
                R"({"@type":"updateFile","file":{"@type":"file","id":991,"size":4,"remote":{"unique_id":"slow-startup"},"local":{"path":"ux0:data/vitagram/files/slow-startup.bin","is_downloading_completed":true}}})" :
                "{\"@type\":\"updateUserStatus\",\"user_id\":" + std::to_string(90001 + i) +
                ",\"status\":{\"@type\":\"userStatusOnline\",\"expires\":2000000000}}";
            startup.incoming_bytes_ += wire.size();
            startup.incoming_.push_back({std::move(wire), sceKernelGetSystemTimeWide()});
        }
        for (const char* wire : {
            R"({"@type":"updateNewChat","chat":{"id":90002,"title":"Live group","positions":[{"list":{"@type":"chatListMain"},"order":100}]}})",
            R"({"@type":"updateNewMessage","message":{"id":5,"chat_id":90002,"content":{"@type":"messageText","text":{"text":"Fresh message"}}}})"}) {
            startup.incoming_bytes_ += std::strlen(wire);
            startup.incoming_.push_back({wire, sceKernelGetSystemTimeWide()});
        }
        pthread_mutex_unlock(&startup.queue_mutex_);
        until = sceKernelGetSystemTimeWide() + 3000000;
        bool drained = false;
        while (sceKernelGetSystemTimeWide() < until) {
            startup.pump(false);
            pthread_mutex_lock(&startup.queue_mutex_);
            drained = startup.incoming_.empty() && startup.parsed_.empty() &&
                startup.user_presence_.size() == 999 && !startup.messages_.empty() && !startup.chats_.empty();
            pthread_mutex_unlock(&startup.queue_mutex_);
            if (drained) break;
            sceKernelDelayThread(1000);
        }
        const bool responsive = drained && !storage_released;
        storage_released = true;
        assert(responsive);
        assert(startup.messages_.size() == 1 && startup.messages_[0].id == 5);
        assert(startup.chats_[0].title == "Live group");
        until = sceKernelGetSystemTimeWide() + 3000000;
        while (startup.user_photos_[90001].path.empty() && sceKernelGetSystemTimeWide() < until) {
            startup.pump(false); sceKernelDelayThread(1000);
        }
        assert(startup.user_photos_[90001].path == slow_media_path);
        assert(startup.uploads_.items().empty()); // Validation never replays protocol events.
        startup.stopWorker(); storage_blocked = false;
        assert(!unlink(slow_media_path));
        assert(startup.messages_.size() == 1); // No message replay after validation.
        std::puts("Startup: 1002 events, chat and new message pass blocked storage; late file-only result restores avatar");
    }

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

    // Polls: parse actual message content, preserve display order separately from
    // API option positions, reject duplicates/closed polls and correlate replies.
    auto* poll_message = parse(R"({"@type":"message","id":55,"chat_id":42,"content":{"@type":"messagePoll","poll":{"id":"77","question":{"text":"Выберите ответ"},"type":{"@type":"pollTypeRegular"},"allows_revoting":true,"allows_multiple_answers":true,"is_anonymous":true,"option_order":[1,0],"options":[{"text":{"text":"Первый"}},{"text":{"text":"Второй"}}]}}})");
    MessageItem poll_item = tg.parseMessage(poll_message); json_decref(poll_message);
    assert(poll_item.poll.id==77 && poll_item.poll.multiple && poll_item.poll.order[0]==1);
    assert(poll_item.text.find("Выберите ответ")!=std::string::npos);
    auto single = poll_item.poll; single.multiple = false;
    assert(!single.validAnswer({0,1}) && single.validAnswer({1}));
    single.options[0].chosen = true; assert(single.validAnswer({}));
    single.restricted = true; assert(!single.validAnswer({0}));
    // A live poll update must also update the persisted history snapshot.
    UiCache cache; cache.load();
    auto* cached_message = parse(R"({"@type":"message","id":55,"chat_id":42,"content":{"@type":"messagePoll","poll":{"id":"77","question":{"text":"Вопрос"}}}})");
    cache.remember(cached_message); json_decref(cached_message);
    auto* update = parse(R"({"@type":"updatePoll","poll":{"id":"77","is_closed":true}})");
    cache.remember(update); json_decref(update);
    auto* cached_history = cache.history(42);
    assert(json_is_true(json_object_get(json_object_get(json_object_get(json_object_get(cached_history,"55"),"content"),"poll"),"is_closed")));
    json_decref(cached_history);
    {
        // Chat eviction happens a few messages per update and never drops the chat being written.
        UiCache evict; evict.load();
        for (int chat = 1; chat <= 30; ++chat)
            for (int id = 1; id <= 40; ++id) {
                std::string text = "{\"@type\":\"updateNewMessage\",\"message\":{\"@type\":\"message\",\"id\":" + std::to_string(id) +
                    ",\"chat_id\":" + std::to_string(chat) + ",\"content\":{\"@type\":\"messageText\"}}}";
                auto* update = parse(text.c_str()); evict.remember(update); json_decref(update);
            }
        auto* last = evict.history(30);
        assert(json_object_size(last) == 40);
        json_decref(last);
        int kept = 0; for (int chat = 1; chat <= 30; ++chat) { auto* h = evict.history(chat); if (json_object_size(h)) ++kept; json_decref(h); }
        assert(kept <= 21 && kept >= 20);
    }
    tg.current_chat_id_=42; tg.current_topic_=0;
    tg.messages_={poll_item}; tg.history_items_={poll_item}; tg.outgoing_.clear();
    assert(!tg.answerPoll(55, {})); assert(!tg.answerPoll(55, {2})); assert(!tg.answerPoll(55, {0,0}));
    assert(tg.answerPoll(55, {1,0})); assert(!tg.answerPoll(55, {0}));
    auto* vote=parse(tg.outgoing_.back().text.c_str());
    assert(MediaUploads::string(vote,"@type")=="setPollAnswer");
    assert(json_integer_value(json_array_get(json_object_get(vote,"option_ids"),0))==1);
    json_decref(vote);
    tg.process(parse(R"({"@type":"error","message":"VOTE_FAILED","@extra":{"kind":"poll_vote","chat":42,"message":55}})"));
    assert(!tg.pollPending(55) && tg.pollStatus(55).find("VOTE_FAILED")!=std::string::npos);
    assert(tg.answerPoll(55,{0}));
    tg.process(parse(R"({"@type":"ok","@extra":{"kind":"poll_vote","chat":42,"message":55}})"));
    assert(tg.pollPending(55));
    auto* refresh=parse(tg.outgoing_.back().text.c_str());
    assert(MediaUploads::string(refresh,"@type")=="getMessage"); json_decref(refresh);
    tg.process(parse(R"({"@type":"message","id":55,"chat_id":42,"@extra":{"kind":"poll_refresh","chat":42,"message":55},"content":{"@type":"messagePoll","poll":{"id":"77","question":{"text":"Викторина"},"type":{"@type":"pollTypeQuiz","correct_option_ids":[0],"explanation":{"text":"Объяснение"}},"allows_revoting":false,"options":[{"text":{"text":"Ответ"},"is_chosen":true,"voter_count":3,"vote_percentage":100}]}}})"));
    assert(!tg.pollPending(55) && tg.messages_[0].poll.options[0].correct);
    assert(tg.messages_[0].poll.explanation=="Объяснение");
    assert(!tg.answerPoll(55,{})); assert(!tg.answerPoll(55,{0}));
    tg.process(parse(R"({"@type":"updatePoll","poll":{"id":"77","question":{"text":"Закрыт"},"is_closed":true,"options":[{"text":"Ответ"}]}})"));
    assert(tg.messages_[0].poll.closed && tg.history_items_[0].poll.closed);
    assert(!tg.answerPoll(55,{0}));
    std::puts("Polls: parsing, multiple answers, order, errors, refresh, quiz and closed-state checks passed");

    // Permission reads during an open menu and Delete must never restart the TTL refresh.
    {
        TelegramClient deletion;
        deletion.ready_ = true; deletion.worker_started_ = true; deletion.current_chat_id_ = 42;
        MessageItem item; item.id = 77;
        deletion.history_items_ = {item};
        assert(!deletion.canDeleteMessage(77));
        assert(deletion.deletePermissionPending(77) && deletion.outgoing_.size() == 1);
        auto reply = [&](bool allowed) {
            auto* response = json_pack("{s:s,s:b,s:{s:s,s:I,s:I,s:I}}", "@type", "messageProperties",
                "can_be_deleted_for_all_users", allowed, "@extra", "kind", "delete_permission",
                "chat", (json_int_t)42, "message", (json_int_t)77,
                "generation", (json_int_t)deletion.history_generation_);
            deletion.process(response);
        };
        reply(true);
        assert(!deletion.deletePermissionPending(77) && deletion.canDeleteMessage(77, false));
        deletion.delete_permissions_[77].checked = sceKernelGetSystemTimeWide() - 6000000;
        auto count = deletion.outgoing_.size();
        for (int frame=0; frame<600; ++frame) assert(deletion.canDeleteMessage(77, false));
        assert(deletion.outgoing_.size() == count); // No request while displaying the snapshot.
        assert(deletion.canDeleteMessage(77)); // Refresh retains the last known result.
        assert(deletion.deletePermissionPending(77) && deletion.outgoing_.size() == count + 1);
        reply(true);
        deletion.delete_permissions_[77].checked = sceKernelGetSystemTimeWide() - 6000000;
        count = deletion.outgoing_.size();
        deletion.deleteMessage(77);
        assert(deletion.outgoing_.size() == count + 1);
        auto* request = parse(deletion.outgoing_.back().text.c_str());
        assert(MediaUploads::string(request, "@type") == "deleteMessages"); json_decref(request);
        deletion.message_delete_pending_ = false;
        reply(false);
        count = deletion.outgoing_.size(); deletion.deleteMessage(77);
        assert(deletion.outgoing_.size() == count && !deletion.canDeleteMessage(77, false));
        // All loaded photo-album members need a completed permission check.
        item.media_album_id = 3; item.photos.push_back({});
        MessageItem other = item; other.id = 78;
        deletion.history_items_ = {item, other}; deletion.delete_permissions_.clear();
        assert(!deletion.canDeleteMessage(77) && deletion.deletePermissionPending(77));
        reply(true);
        assert(deletion.deletePermissionPending(77) && !deletion.canDeleteMessage(77, false));
        deletion.delete_permissions_[78].pending = false;
        deletion.delete_permissions_[78].allowed = true;
        assert(!deletion.deletePermissionPending(77) && deletion.canDeleteMessage(77, false));
        deletion.worker_started_ = false; deletion.ready_ = false;
        std::puts("Deletion permissions: stable cached reads, TTL refresh, direct delete, denial and album pending checks passed");
    }

    auto* video_message=parse(R"({"@type":"message","id":66,"chat_id":42,"content":{"@type":"messageVideo","video":{"width":1920,"height":1080,"duration":20,"video":{"id":100,"size":200000000,"local":{"is_downloading_completed":true,"path":"original.mp4"}}},"alternative_videos":[{"width":640,"height":360,"codec":"h264","video":{"id":101,"size":2000000}},{"width":320,"height":180,"codec":"h264","video":{"id":102,"size":1000000}},{"width":160,"height":90,"codec":"av1","video":{"id":103,"size":500000}},{"width":320,"height":180,"codec":"h264","video":{"id":104,"size":800000},"hls_file":{"id":999}}]}})");
    // Preference caps select the largest eligible raster; ties keep the smaller file.
    for (int quality = 1; quality <= 3; ++quality) {
        tg.setVideoQuality(quality);
        assert(tg.parseMessage(video_message).animation_file_id == 101);
    }
    {
        auto* original_only = parse(R"({"content":{"@type":"messageVideo","video":{"width":854,"height":480,"video":{"id":200,"size":1000000}},"alternative_videos":[{"width":1280,"height":720,"codec":"h264","video":{"id":201,"size":2000000}}]}})");
        tg.setVideoQuality(1); // No variant below the maximum: do not download a larger one.
        const auto blocked = tg.parseMessage(original_only);
        assert(!blocked.animation_error.empty() && blocked.video_variants.empty());
        tg.setVideoQuality(2);
        assert(tg.parseMessage(original_only).animation_file_id == 200);
        tg.setVideoQuality(3);
        assert(tg.parseMessage(original_only).animation_file_id == 201);
        json_decref(original_only);
    }
    auto* quality_content = json_object_get(video_message, "content");
    auto* quality_variants = json_object_get(quality_content, "alternative_videos");
    json_array_append_new(quality_variants, parse(R"({"width":854,"height":480,"codec":"h264","video":{"id":105,"size":3000000}})"));
    json_array_append_new(quality_variants, parse(R"({"width":1280,"height":720,"codec":"h264","video":{"id":106,"size":4000000}})"));
    json_array_append_new(quality_variants, parse(R"({"width":720,"height":1280,"codec":"h264","video":{"id":107,"size":1000000}})"));
    {
        const auto qualities = VideoQuality::available(quality_content, 3);
        assert(qualities.size() == 4 && qualities.back().width == 1280 && qualities.back().height == 720);
        const auto full_hd = VideoQuality{json_object_get(json_object_get(quality_content,"video"),"video"),1920,1080,false};
        assert(!std::strcmp(VideoQuality::exclusion(full_hd,3),"hardware_raster"));
        auto* highest = json_array_get(quality_variants,5);
        const VideoQuality q{json_object_get(highest,"video"),1280,720,true};
        assert(!VideoQuality::exclusion(q,3));
        assert(!std::strcmp(VideoQuality::exclusion(q,2),"quality_limit"));
        FILE* log=std::tmpfile(); assert(log);
        VideoQuality::diagnose(log,quality_content,3);
        std::rewind(log);
        char buffer[4096]{};
        const auto bytes=std::fread(buffer,1,sizeof(buffer)-1,log);
        assert(bytes && std::strstr(buffer,"width=1280 height=720 bytes=4000000 h264=1 result=allowed"));
        assert(std::strstr(buffer,"result=unsupported_codec") && std::strstr(buffer,"result=hardware_raster"));
        std::fclose(log);
    }
    {
        TelegramClient client;
        client.video_quality_ = 3;
        const auto mask = diag_log::mask().load();
        diag_log::set_mask(mask | (1u << static_cast<unsigned>(diag_log::Kind::Video)));
        const int opened = quality_opens, stated = quality_stats;
        assert(client.parseMessage(video_message).animation_file_id == 106);
        assert(quality_opens == opened && quality_stats == stated && client.video_quality_logs_.size() == 1);
        assert(client.video_quality_logs_.front().find("width=1280 height=720") != std::string::npos);
        quality_blocked = true; quality_entered = quality_released = false;
        std::thread disk([&] { client.flushVideoQualityLogs(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!quality_entered && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto parsed = std::async(std::launch::async, [&] { return client.parseMessage(video_message).animation_file_id; });
        const bool responsive = parsed.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
        quality_released = true; disk.join(); quality_blocked = false;
        assert(quality_entered && responsive && parsed.get() == 106);
        assert(client.video_quality_logs_.size() == 1); // Enqueue during blocked output belongs to the next batch.
        client.flushVideoQualityLogs();
        assert(client.video_quality_logs_.empty());
        FILE* log = __real_fopen("ux0:data/vitagram/video-quality.log", "rb"); assert(log);
        std::string output; char line[512]; while (std::fgets(line, sizeof(line), log)) output += line;
        std::fclose(log);
        assert(output.find("message=66 maximum=3") != std::string::npos);
        assert(output.find("width=1280 height=720") != std::string::npos);
        for (int i = 0; i < 70; ++i) client.parseMessage(video_message);
        assert(client.video_quality_logs_.size() == 64);
        auto* huge = json_deep_copy(quality_content);
        auto* variants = json_object_get(huge, "alternative_videos");
        for (int i = 0; i < 200; ++i) json_array_append(variants, json_array_get(variants, 0));
        const auto bounded = VideoQuality::diagnostic(huge, 3);
        assert(bounded.size() <= 8192 && bounded.find("omitted_variants=1") != std::string::npos);
        json_decref(huge);
        diag_log::set_mask(mask & ~(1u << static_cast<unsigned>(diag_log::Kind::Video)));
        const int before_disable = quality_opens;
        client.flushVideoQualityLogs();
        client.parseMessage(video_message);
        assert(client.video_quality_logs_.empty() && quality_opens == before_disable);
        diag_log::set_mask(mask);
        std::puts("Video quality log: no UI disk access, stalled writer isolation, bounded snapshots and disabled logging passed");
    }
    for (int quality = 0; quality <= 3; ++quality) {
        tg.setVideoQuality(quality);
        const int files[] = {104, 101, 105, 106};
        assert(tg.parseMessage(video_message).animation_file_id == files[quality]);
    }
    {
        TelegramClient player;
        player.ready_ = true; player.worker_started_ = true; player.current_chat_id_ = 42;
        player.setVideoQuality(3);
        const auto full = player.parseMessage(video_message);
        assert(full.animation_file_id == 106 && full.video_variants.size() == 4);
        player.messages_ = player.history_items_ = {full};
        player.setAnimationTarget(0);
        player.outgoing_.clear();
        assert(player.selectVideoQuality(0, 1));
        assert(player.messages_[0].animation_file_id == 101 && player.history_items_[0].animation_file_id == 101);
        player.setAnimationTarget(0);
        assert(player.outgoing_.size() == 2);
        auto* cancelled = parse(player.outgoing_[0].text.c_str());
        auto* requested = parse(player.outgoing_[1].text.c_str());
        assert(MediaUploads::string(cancelled, "@type") == "cancelDownloadFile" && MediaUploads::number(cancelled, "file_id") == 106);
        assert(MediaUploads::string(requested, "@type") == "downloadFile" && MediaUploads::number(requested, "file_id") == 101);
        json_decref(cancelled); json_decref(requested);
        assert(player.parseMessage(video_message).animation_file_id == 101);
        assert(!player.selectVideoQuality(-1, 0) && !player.selectVideoQuality(0, 4) && !player.selectVideoQuality(0, 1));
        auto* extra_quality = json_deep_copy(video_message);
        auto* extra_alts = json_object_get(json_object_get(extra_quality, "content"), "alternative_videos");
        json_array_append_new(extra_alts, parse(R"({"width":426,"height":240,"codec":"h264","video":{"id":108,"size":1500000}})"));
        const auto all = player.parseMessage(extra_quality);
        assert(all.video_variants.size() == 5 && all.animation_file_id == 101);
        assert(all.video_variants[1].height == 240); // Offer intermediate qualities too, not only fixed presets.
        json_decref(extra_quality);
        player.setVideoQuality(1);
        assert(!player.selectVideoQuality(0, 3));
        const auto capped = player.parseMessage(video_message);
        assert(capped.animation_file_id == 101 && capped.video_variants.size() == 2);
        player.messages_[0].animation_error = "download grew beyond the size limit";
        assert(player.selectVideoQuality(0, 0));
        assert(player.messages_[0].animation_file_id == 104 && player.messages_[0].animation_error.empty());
        json_object_set_new(quality_content, "is_secret", json_true());
        const auto secret = player.parseMessage(video_message);
        assert(!secret.animation_error.empty() && secret.video_variants.empty());
        json_object_del(quality_content, "is_secret");
        json_object_set_new(video_message, "chat_id", json_integer(99));
        assert(player.parseMessage(video_message).animation_file_id == 101); // Other chats keep their own choice.
        json_object_set_new(video_message, "chat_id", json_integer(42));
        player.ready_ = false; player.worker_started_ = false;
        std::puts("Player quality: per-message selection, maximum, deduplication, history, cancellation and download IDs passed");
    }
    // Metadata from the reported 720p stream must reach download and stay valid on file updates.
    {
        auto* large = json_deep_copy(video_message);
        auto* content = json_object_get(large,"content");
        auto* alternatives = json_object_get(content,"alternative_videos");
        auto* file = json_object_get(json_array_get(alternatives,5),"video");
        json_object_set_new(file,"size",json_integer(170190135));
        TelegramClient player;
        player.ready_=true; player.worker_started_=true; player.current_chat_id_=42;
        player.setVideoQuality(3);
        const auto message=player.parseMessage(large);
        assert(message.animation_file_id==106 && message.animation_error.empty());
        player.messages_=player.history_items_={message};
        player.setAnimationTarget(0);
        const auto old_diag_mask = diag_log::mask().load();
        diag_log::set_mask(old_diag_mask | (1u << static_cast<unsigned>(diag_log::Kind::Performance)));
        player.messages_[0].animation_stream = std::make_shared<download_stream::State>(106);
        player.messages_[0].animation_stream->update("large.mp4", 170190135, 0, 0, false);
        player.setAnimationTarget(0);
        player.animation_rate_at_ -= 1000000;
        player.messages_[0].animation_downloaded = 1024;
        player.messages_[0].animation_stream->update("large.mp4", 170190135, 0, 512, false);
        performance::collector().take(performance::now());
        player.setAnimationTarget(0);
        auto rates = performance::collector().take(performance::now());
        assert(rates.get(performance::Metric::VideoDownloadRate).maximum > 900);
        assert(rates.get(performance::Metric::VideoPrefixRate).maximum > 450);
        player.animation_rate_at_ -= 1000000;
        player.setAnimationTarget(0);
        rates = performance::collector().take(performance::now());
        assert(rates.get(performance::Metric::VideoDownloadRate).count == 1 &&
               rates.get(performance::Metric::VideoDownloadRate).maximum == 0);
        diag_log::set_mask(old_diag_mask);
        auto* request=parse(player.outgoing_.back().text.c_str());
        assert(MediaUploads::number(request,"file_id")==106);
        json_decref(request);
        auto* update=parse(R"({"@type":"updateFile","file":{"@type":"file","id":106,"size":170190135,"local":{"path":"large.mp4","downloaded_size":170190135,"is_downloading_completed":true}}})");
        player.process(update); json_decref(update);
        assert(player.messages_[0].animation_error.empty() && player.messages_[0].animation_downloaded==170190135);
        json_object_set_new(file,"size",json_integer(media_limits::video_bytes));
        assert(player.parseMessage(large).animation_error.empty());
        json_object_set_new(file,"size",json_integer(media_limits::video_bytes+1));
        assert(player.parseMessage(large).animation_file_id!=106);
        player.ready_=false; player.worker_started_=false;
        json_decref(large);
    }
    // Over-limit files and unsupported codecs cannot win at the highest preference.
    auto* highest = json_object_get(json_array_get(quality_variants, 5), "video");
    json_object_set_new(highest, "size", json_integer(media_limits::video_bytes + 1));
    assert(tg.parseMessage(video_message).animation_file_id == 105);
    json_object_set_new(json_array_get(quality_variants, 4), "codec", json_string("av1"));
    assert(tg.parseMessage(video_message).animation_file_id == 101);
    for (int i = 0; i < 3; ++i) json_array_remove(quality_variants, 4);
    tg.setVideoQuality(0);
    auto small=tg.parseMessage(video_message);
    assert(small.animation_file_id==104 && small.video_width==320 && small.video_height==180);
    assert(small.animation_size==800000 && small.animation_error.empty() && small.animation_path.empty() && small.video_alternative);
    tg.messages_={small}; tg.setAnimationTarget(0);
    auto* download=parse(tg.outgoing_.back().text.c_str());
    assert(MediaUploads::string(download,"@type")=="downloadFile" && MediaUploads::number(download,"file_id")==104);
    json_decref(download);
    {
        TelegramClient background;
        background.ready_ = true; background.worker_started_ = true;
        background.messages_ = {small};
        background.setAnimationTarget(0);
        background.setBackgroundMedia(0, small.animation_file_id);
        background.outgoing_.clear();
        background.setAnimationTarget(-1);
        assert(background.outgoing_.empty());
        assert(background.pending_files_.count(small.animation_file_id));
        background.setBackgroundMedia(0, 0);
        assert(background.outgoing_.size() == 1);
        auto* cancel = parse(background.outgoing_.back().text.c_str());
        assert(MediaUploads::string(cancel, "@type") == "cancelDownloadFile");
        assert(MediaUploads::number(cancel, "file_id") == small.animation_file_id);
        json_decref(cancel);
        background.ready_ = false; background.worker_started_ = false;
        std::puts("Background video: navigation keeps downloading; closing the player cancels the retired download");
    }
    auto* video_content=json_object_get(video_message,"content");
    // Spoiler consent gates full media, including the minimum-quality alternative.
    {
        TelegramClient hidden;
        json_object_set_new(video_content, "has_spoiler", json_true());
        auto* original_video = json_object_get(video_content, "video");
        json_object_set_new(original_video, "thumbnail", parse(R"({"format":{"@type":"thumbnailFormatJpeg"},"file":{"id":501}})"));
        json_object_set_new(original_video, "minithumbnail", parse(R"({"data":"/9j/"})"));
        auto item = hidden.parseMessage(video_message);
        assert(item.spoiler && item.media_file_id == 501 && !item.media_fallback.empty());
        hidden.ready_ = true; hidden.worker_started_ = true;
        hidden.messages_ = {item};
        hidden.setAnimationTarget(0);
        assert(hidden.outgoing_.empty() && hidden.active_animation_file_ == 0);
        hidden.ensureMessageMedia(0);
        assert(hidden.outgoing_.size() == 1);
        auto* request = parse(hidden.outgoing_.back().text.c_str());
        assert(MediaUploads::number(request, "file_id") == 501); json_decref(request);
        hidden.setAnimationTarget(0, true);
        request = parse(hidden.outgoing_.back().text.c_str());
        assert(MediaUploads::string(request, "@type") == "downloadFile" && MediaUploads::number(request, "file_id") == 104);
        json_decref(request);
        hidden.setAnimationTarget(0, false);
        request = parse(hidden.outgoing_.back().text.c_str());
        assert(MediaUploads::string(request, "@type") == "cancelDownloadFile" && MediaUploads::number(request, "file_id") == 104);
        json_decref(request);
        assert(hidden.active_animation_file_ == 0);
        hidden.ready_ = false; hidden.worker_started_ = false;
        json_object_set_new(video_content, "has_spoiler", json_false());
        std::puts("Video spoilers: blurred preview metadata, thumbnail-only download, explicit consent and cancellation passed");
    }

    auto* alternatives=json_object_get(video_content,"alternative_videos");
    auto* cached_file=json_object_get(json_array_get(alternatives,3),"video");
    json_object_set_new(cached_file,"id",json_integer(0));
    json_object_set_new(cached_file,"local",parse(R"({"is_downloading_completed":true,"path":"small.mp4"})"));
    assert(tg.parseMessage(video_message).animation_path=="small.mp4");
    json_object_del(video_content,"alternative_videos");
    auto original=tg.parseMessage(video_message);
    assert(original.animation_file_id==100 && !original.video_alternative && !original.animation_error.empty());
    // Unknown size still permits the lowest resolution; unavailable files do not.
    json_object_set_new(video_content,"alternative_videos",parse(R"([{"width":256,"height":144,"codec":"h264","video":{"id":110}},{"width":160,"height":90,"codec":"h264","video":{"id":0}},{"width":0,"height":0,"codec":"h264","video":{"id":111}}])"));
    for (int quality = 0; quality <= 3; ++quality) {
        tg.setVideoQuality(quality);
        assert(tg.parseMessage(video_message).animation_file_id == 110);
    }
    tg.setVideoQuality(0);
    json_decref(video_message);
    auto* circle=parse(R"({"content":{"@type":"messageVideoNote","video_note":{"length":240,"video":{"id":120,"size":1000}}}})");
    auto note=tg.parseMessage(circle); json_decref(circle);
    assert(note.animation_file_id==120 && note.video_width==240 && !note.video_alternative);
    std::puts("Video quality: lowest resolution, size tie-break, codecs, cache, limits, file request and circle fallback passed");

    // GIF cards have an embedded preview before any animation/thumbnail download.
    {
        TelegramClient gifs;
        auto* message = parse(R"({"@type":"message","id":67,"chat_id":42,"content":{"@type":"messageAnimation","animation":{"width":320,"height":180,"animation":{"id":130,"size":1000},"minithumbnail":{"data":"/9j/"},"thumbnail":{"format":{"@type":"thumbnailFormatMpeg4"},"file":{"id":131}}}}})");
        auto item = gifs.parseMessage(message);
        assert(item.animation && !item.video && item.media_fallback == "inline-jpeg:/9j/");
        assert(item.media_path.empty() && item.animation_path.empty());
        assert(item.media_video_preview && item.media_file_id == 131);
        gifs.messages_ = {item}; gifs.ensureMessageMedia(0);
        assert(gifs.outgoing_.empty()); // Embedded JPEG requires no network request.
        gifs.ready_ = true; gifs.worker_started_ = true;
        gifs.history_items_ = {item}; gifs.ensureMessageMedia(0); gifs.ensureMessageMedia(0);
        assert(gifs.outgoing_.size() == 1); // Visible card downloads just the thumbnail, once.
        auto* preview_request = parse(gifs.outgoing_.back().text.c_str());
        assert(MediaUploads::string(preview_request, "@type") == "downloadFile");
        assert(MediaUploads::number(preview_request, "file_id") == 131);
        json_decref(preview_request); gifs.outgoing_.clear();
        gifs.process(parse(R"({"@type":"updateFile","file":{"id":131,"local":{"is_downloading_completed":true,"path":"card-thumb.mp4"}}})"));
        assert(gifs.messages_[0].media_path == "video-thumb:card-thumb.mp4");
        assert(gifs.history_items_[0].media_path == "video-thumb:card-thumb.mp4");
        assert(gifs.messages_[0].animation_path.empty());
        gifs.ensureMessageMedia(0); assert(gifs.outgoing_.empty());
        gifs.ready_ = false; gifs.worker_started_ = false; gifs.history_items_.clear();
        auto* content = json_object_get(message, "content");
        auto* animation = json_object_get(content, "animation");
        json_object_set_new(json_object_get(json_object_get(animation, "thumbnail"), "file"), "local", parse(R"({"is_downloading_completed":true,"path":"ready.mp4"})"));
        assert(gifs.parseMessage(message).media_path == "video-thumb:ready.mp4");
        // A GIF without an embedded JPEG still downloads its separate thumbnail.
        json_object_del(animation, "minithumbnail");
        item = gifs.parseMessage(message);
        assert(item.media_fallback.empty() && item.media_video_preview && item.media_file_id == 131);
        json_object_set_new(animation, "minithumbnail", parse(R"({"data":"/9j/"})"));
        json_object_set_new(animation, "thumbnail", parse(R"({"format":{"@type":"thumbnailFormatJpeg"},"file":{"id":132,"local":{"is_downloading_completed":true,"path":"clear.jpg"}}})"));
        item = gifs.parseMessage(message);
        assert(item.media_path == "clear.jpg" && item.media_fallback == "inline-jpeg:/9j/");
        json_object_set_new(animation, "minithumbnail", parse(R"({"data":"bad"})"));
        assert(gifs.parseMessage(message).media_fallback.empty());
        json_object_del(animation, "minithumbnail");
        assert(gifs.parseMessage(message).media_fallback.empty());
        json_object_set_new(animation, "minithumbnail", parse(R"({"data":"/9j/"})"));
        gifs.current_chat_id_ = 42;
        auto* update = json_pack("{s:s,s:O}", "@type", "updateNewMessage", "message", message);
        gifs.ui_cache_.remember(update); json_decref(update); json_decref(message);
        gifs.messages_.clear();
        gifs.requestCachedDetails(); gifs.prepareCachedDetails(); gifs.applyCachedDetails();
        assert(gifs.messages_.size() == 1 && gifs.messages_[0].media_fallback == "inline-jpeg:/9j/");
    }
    std::puts("GIF cards: embedded preview, visible MP4 thumbnail requests, file updates, static thumbnail and cached history passed");

    auto* rich=parse(R"({"content":{"@type":"messageRichMessage","message":{"blocks":[{"@type":"pageBlockTitle","title":{"@type":"richTextBold","text":{"@type":"richTextPlain","text":"Album"}}},{"@type":"pageBlockCollage","blocks":[{"@type":"pageBlockPhoto","photo":{"sizes":[{"width":320,"height":240,"photo":{"id":201,"size":1000}}]}},{"@type":"pageBlockPhoto","photo":{"sizes":[{"width":640,"height":480,"photo":{"id":202,"size":2000}}]}}]},{"@type":"pageBlockVideo","video":{"width":320,"height":180,"duration":4,"video":{"id":203,"size":3000}}},{"@type":"pageBlockList","items":[{"label":"• ","blocks":[{"@type":"pageBlockParagraph","text":{"@type":"richTextUrl","url":"https://example.org/hidden","text":{"@type":"richTextPlain","text":"Description"}}}]}]}]}}})");
    auto rich_item=tg.parseMessage(rich); json_decref(rich);
    assert(rich_item.text=="Album\n• Description" && rich_item.media_label=="2 фото · Видео");
    assert(rich_item.photos.size()==2 && rich_item.photos[0].thumbnail_file_id==201 && rich_item.photos[1].full_file_id==202);
    assert(rich_item.animation && rich_item.video && rich_item.animation_file_id==203);
    assert(rich_item.links.size()==1 && rich_item.links[0]=="https://example.org/hidden");
    std::puts("Rich messages: nested text, multiple photos and video passed");
    auto* pin_message=parse(R"({"id":601,"content":{"@type":"messagePinMessage","message_id":501}})");
    auto pin_item=tg.parseMessage(pin_message); json_decref(pin_message);
    assert(pin_item.text=="Закреплено сообщение" && pin_item.media_label.empty());
    assert(pin_item.pin_message && pin_item.pinned_message_id==501);
    assert(pin_item.service_message);
    auto* joined_message=parse(R"({"id":602,"sender_id":{"@type":"messageSenderUser","user_id":5},"content":{"@type":"messageChatJoinByLink"}})");
    auto joined_item=tg.parseMessage(joined_message); json_decref(joined_message);
    assert(joined_item.service_message && !joined_item.pin_message && !joined_item.pinned_message_id);
    assert(joined_item.text.find("Вступление по ссылке: ")==0 && joined_item.text.find(tg.senderName(joined_item))!=std::string::npos);
    {
        TelegramClient members;
        members.user_names_[5]="Admin"; members.user_names_[1234567890123LL]="Member";
        auto* removed=parse(R"({"sender_id":{"@type":"messageSenderUser","user_id":5},"content":{"@type":"messageChatDeleteMember","user_id":1234567890123}})");
        auto removed_item=members.parseMessage(removed); json_decref(removed);
        assert(removed_item.service_message && removed_item.member_removed && removed_item.removed_member_id==1234567890123LL);
        assert(removed_item.text=="Admin удалил(а) Member из чата");
        removed_item.sender_id=removed_item.removed_member_id;
        assert(members.serviceMessageText(removed_item)=="Member покинул(а) чат");
        removed_item.sender_id=5;
        members.user_names_.erase(removed_item.removed_member_id);
        members.current_chat_id_=42; members.ready_=true; members.worker_started_=true;
        members.messages_={removed_item}; members.ensureMessageMedia(0);
        assert(members.outgoing_.size()==1);
        auto* lookup=parse(members.outgoing_.back().text.c_str());
        assert(MediaUploads::string(lookup,"@type")=="getUser" && MediaUploads::number(lookup,"user_id")==1234567890123LL);
        json_decref(lookup);
        members.ensureMessageMedia(0); assert(members.outgoing_.size()==1);
        members.user_names_[1234567890123LL]="Updated";
        assert(members.serviceMessageText(removed_item)=="Admin удалил(а) Updated из чата");
        members.worker_started_=false;
    }
    {
        TelegramClient pins;
        pins.current_chat_id_=42; pins.ready_=true; pins.worker_started_=true;
        pins.process(parse(R"({"@type":"updateNewMessage","message":{"id":501,"chat_id":42,"content":{"@type":"messageText","text":{"text":"Pinned text"}}}})"));
        assert(pins.openPinnedMessage(501) && pins.takeMessageJump()==501);
        assert(pins.outgoing_.empty() && pins.takeMessageJump()==0);
        assert(pins.openPinnedMessage(502));
        auto* request=parse(pins.outgoing_.back().text.c_str());
        assert(MediaUploads::string(request,"@type")=="getMessage" && MediaUploads::number(request,"message_id")==502);
        json_decref(request);
        pins.process(parse(R"({"@type":"message","id":502,"chat_id":42,"content":{"@type":"messageText","text":{"text":"Fetched pinned text"}},"@extra":{"kind":"pin_target","chat":42,"generation":0}})"));
        assert(pins.takeMessageJump()==502);
        pins.process(parse(R"({"@type":"message","id":503,"chat_id":42,"content":{"@type":"messageText","text":{"text":"Stale"}},"@extra":{"kind":"pin_target","chat":42,"generation":99}})"));
        assert(pins.takeMessageJump()==0 && pins.messages_.size()==2);
        pins.process(parse(R"({"@type":"error","message":"MESSAGE_NOT_FOUND","@extra":{"kind":"pin_target","chat":42,"generation":0}})"));
        assert(pins.takeMessageJump()==0 && !pins.updateNotice().empty());
        pins.worker_started_=false;
    }

    TelegramClient speech;
    speech.current_chat_id_=42;
    speech.process(parse(R"({"@type":"updateNewMessage","message":{"id":501,"chat_id":42,"content":{"@type":"messageVoiceNote","voice_note":{"duration":8,"voice":{"id":50},"speech_recognition_result":{"@type":"speechRecognitionResultPending","partial_text":"Привет"}}}}})"));
    assert(speech.messages_.size()==1 && speech.messages_[0].transcription=="Привет" && speech.messages_[0].transcription_pending);
    speech.process(parse(R"({"@type":"updateMessageContent","chat_id":42,"message_id":501,"new_content":{"@type":"messageVoiceNote","voice_note":{"duration":8,"voice":{"id":50},"speech_recognition_result":{"@type":"speechRecognitionResultText","text":"Привет, мир"}}}})"));
    assert(speech.messages_[0].transcription=="Привет, мир" && !speech.messages_[0].transcription_pending);
    auto* transcribed_circle=parse(R"({"content":{"@type":"messageVideoNote","video_note":{"length":240,"video":{"id":120},"speech_recognition_result":{"@type":"speechRecognitionResultText","text":"Видео текст"}}}})");
    assert(speech.parseMessage(transcribed_circle).transcription=="Видео текст");
    json_decref(transcribed_circle);
    speech.ready_=true; speech.worker_started_=true;
    speech.process(parse(R"({"@type":"updateNewMessage","message":{"id":502,"chat_id":42,"content":{"@type":"messageVoiceNote","voice_note":{"duration":8,"voice":{"id":51}}}}})"));
    assert(speech.requestTranscription(502));
    assert(!speech.requestTranscription(502));
    auto* recognition_request=parse(speech.outgoing_.back().text.c_str());
    assert(MediaUploads::string(recognition_request,"@type")=="recognizeSpeech" && MediaUploads::number(recognition_request,"message_id")==502);
    json_decref(recognition_request);
    speech.process(parse(R"({"@type":"error","message":"PREMIUM_REQUIRED","@extra":{"kind":"speech_request","chat":42,"message":502}})"));
    assert(speech.requestTranscription(502));

    speech.process(parse(R"({"@type":"updateMessageContent","chat_id":42,"message_id":502,"new_content":{"@type":"messageVoiceNote","voice_note":{"speech_recognition_result":{"@type":"speechRecognitionResultText","text":""}}}})"));
    assert(speech.messages_.back().transcription.empty() && speech.messages_.back().transcription_complete);
    assert(!speech.messages_.back().transcription_pending && !speech.requestTranscription(502));
    auto* blank_speech=parse(R"({"content":{"@type":"messageVoiceNote","voice_note":{"speech_recognition_result":{"@type":"speechRecognitionResultText","text":" \n\t"}}}})");
    auto blank_item=speech.parseMessage(blank_speech); json_decref(blank_speech);
    assert(blank_item.transcription_complete && blank_item.transcription.empty());
    speech.worker_started_=false;
    tg.ready_=true; tg.outgoing_.clear();
    assert(tg.openTelegramLink("https://t.me/example_channel/123"));
    auto* link_request=parse(tg.outgoing_.back().text.c_str());
    assert(MediaUploads::string(link_request,"@type")=="searchPublicChat" &&
           MediaUploads::string(link_request,"username")=="example_channel");
    assert(!tg.openTelegramLink("https://t.me/joinchat/secret") && !tg.openTelegramLink("https://evil.test/t.me/example_channel"));
    auto* link_result = parse(R"({"@type":"chat","id":991,"title":"Example","type":{"@type":"chatTypeSupergroup","supergroup_id":77}})");
    json_object_set(link_result, "@extra", json_object_get(link_request, "@extra"));
    json_decref(link_request); tg.process(link_result);
    assert(tg.takeLinkChatOpened() && tg.currentChatId()==991);
    assert(!tg.takeLinkChatOpened());

    // Small sender avatars: only visible senders trigger metadata/downloads.
    ChatItem group; group.id=42; group.group=true; tg.known_chats_={group};
    tg.current_chat_id_=42; tg.ready_=true; tg.messages_={}; tg.pending_files_.clear(); tg.outgoing_.clear();
    MessageItem sender; sender.id=70; sender.sender_id=21; tg.messages_={sender};
    tg.ensureMessageMedia(0); assert(tg.outgoing_.size()==1);
    auto* lookup=parse(tg.outgoing_.back().text.c_str());
    assert(MediaUploads::string(lookup,"@type")=="getUser"); json_decref(lookup);
    tg.ensureMessageMedia(0); assert(tg.outgoing_.size()==1); // Retry backoff.
    auto* user=parse(R"({"id":21,"first_name":"ⲕⲗυⲙⲁⲧ","profile_photo":{"small":{"id":201},"big":{"id":202}}})");
    tg.parseUser(user); json_decref(user);
    assert(tg.senderName(sender)=="ⲕⲗυⲙⲁⲧ");
    tg.ensureMessageMedia(0);
    auto* avatar_request=parse(tg.outgoing_.back().text.c_str());
    assert(MediaUploads::string(avatar_request,"@type")=="downloadFile" && MediaUploads::number(avatar_request,"file_id")==201);
    json_decref(avatar_request);
    auto count=tg.outgoing_.size(); tg.ensureMessageMedia(0); assert(count==tg.outgoing_.size());
    auto* avatar_file=parse(R"({"id":201,"local":{"path":"avatar.jpg","is_downloading_completed":true}})");
    tg.updateFile(avatar_file); json_decref(avatar_file); assert(tg.senderAvatar(sender)=="avatar.jpg");
    avatar_file=parse(R"({"id":201,"local":{"path":"","is_downloading_completed":false}})");
    tg.updateFile(avatar_file); json_decref(avatar_file); assert(tg.senderAvatar(sender).empty());
    user=parse(R"({"id":21,"first_name":"の一部 宇宙","profile_photo":null})");
    tg.parseUser(user); json_decref(user);
    count=tg.outgoing_.size(); tg.ensureMessageMedia(0); assert(count==tg.outgoing_.size()); // No photo: no repeated lookups.
    tg.user_photos_[21].fresh=false; tg.sender_requests_.clear(); tg.ensureMessageMedia(0);
    assert(count+1==tg.outgoing_.size()); // Cached metadata is refreshed for this session.
    MessageItem channel_sender=sender; channel_sender.sender_chat=true; channel_sender.sender_id=22;
    ChatItem channel; channel.id=22; channel.avatar_file_id=203; channel.avatar_path="channel.jpg"; tg.known_chats_.push_back(channel);
    assert(tg.senderAvatar(channel_sender)=="channel.jpg");
    // Deleted users can carry blank or stale names/photos; the TDLib type wins.
    user=parse(R"({"id":21,"first_name":"Old name","type":{"@type":"userTypeDeleted"},"profile_photo":{"small":{"id":201,"local":{"path":"old.jpg","is_downloading_completed":true}}}})");
    tg.parseUser(user); json_decref(user);
    assert(tg.senderIsDeleted(sender) && tg.senderName(sender)==tr("user.deleted"));
    assert(tg.senderAvatar(sender).empty() && tg.user_photos_[21].file==0);
    assert(!tg.senderIsDeleted(channel_sender));
    ChatItem deleted_chat; deleted_chat.id=23; deleted_chat.user_id=21;
    assert(tg.chatIsDeleted(deleted_chat) && tg.chatTitle(deleted_chat)==tr("user.deleted"));
    tg.known_chats_.push_back(deleted_chat); tg.current_chat_id_=23;
    assert(tg.currentChatIsDeleted() && tg.currentChatTitle()==tr("user.deleted"));
    tg.current_chat_id_=42;
    user=parse(R"({"id":21,"first_name":" \t ","last_name":" "})");
    tg.parseUser(user); json_decref(user);
    assert(tg.senderIsDeleted(sender) && tg.senderName(sender)==tr("user.deleted"));
    user=parse(R"({"id":21,"first_name":"  Restored  ","last_name":" User ","type":{"@type":"userTypeRegular"}})");
    tg.parseUser(user); json_decref(user);
    assert(!tg.senderIsDeleted(sender) && tg.senderName(sender)=="Restored User");
    user=parse(R"({"id":21,"first_name":" ","type":{"@type":"userTypeUnknown"}})");
    tg.parseUser(user); json_decref(user);
    assert(!tg.senderIsDeleted(sender) && tg.senderName(sender)==tr("user.participant"));
    std::puts("Avatars: visible sender lookup, small file, deduplication, updates, deletion, cache refresh and chat senders passed");
    tg.worker_started_=false; std::remove("upload-test.bin");
    {
        TelegramClient packs;
        packs.worker_started_=true; packs.ready_=true; packs.current_chat_id_=42;
        auto* raw = parse(R"({"id":5,"edit_date":123,"content":{"@type":"messageSticker","sticker":{"set_id":"77","format":{"@type":"stickerFormatWebp"}}}})");
        const auto sticker = packs.parseMessage(raw); json_decref(raw);
        assert(sticker.sticker_set_id == 77 && sticker.edit_date == 123);
        raw = parse(R"({"content":{"@type":"messageText","text":{"text":"😀","entities":[{"offset":0,"length":2,"type":{"@type":"textEntityTypeCustomEmoji","custom_emoji_id":"999"}}]}}})");
        const auto emoji = packs.parseMessage(raw); json_decref(raw);
        assert(emoji.custom_emoji_ids == std::vector<int64_t>{999});
        packs.openStickerSet(0, 999);
        assert(!packs.stickerSetCanInstall());
        auto* request = parse(packs.outgoing_.back().text.c_str());
        assert(MediaUploads::string(request,"@type") == "getCustomEmojiStickers");
        auto* response = parse(R"({"@type":"stickers","stickers":[{"set_id":"77"}]})");
        json_object_set(response,"@extra",json_object_get(request,"@extra")); json_decref(request);
        packs.process(response);
        request = parse(packs.outgoing_.back().text.c_str());
        assert(MediaUploads::string(request,"@type") == "getStickerSet" && MediaUploads::number(request,"set_id") == 77);
        auto* extra = json_incref(json_object_get(request,"@extra")); json_decref(request);
        assert(!packs.stickerSetCanInstall());
        response = parse(R"({"@type":"stickerSet","id":"77","title":"Emoji pack","is_installed":false,"sticker_type":{"@type":"stickerTypeCustomEmoji"},"stickers":[{"set_id":"77","format":{"@type":"stickerFormatWebp"},"sticker":{"id":9}}]})");
        json_object_set(response,"@extra",extra); packs.process(response);
        assert(packs.stickerSetTitle() == "Emoji pack" && packs.stickerSetCanInstall());
        assert(packs.stickers().size() == 1 && packs.stickers()[0].set_id == 77 && !packs.sendSticker(0));
        packs.installStickerSet(); assert(!packs.stickerSetCanInstall());
        request = parse(packs.outgoing_.back().text.c_str());
        assert(MediaUploads::string(request,"@type") == "changeStickerSet" && json_is_true(json_object_get(request,"is_installed")));
        auto requests = packs.outgoing_.size(); packs.installStickerSet(); assert(requests == packs.outgoing_.size());
        response = parse(R"({"@type":"error","message":"Try again"})");
        json_object_set(response,"@extra",json_object_get(request,"@extra")); packs.process(response);
        assert(packs.stickerSetCanInstall() && packs.stickerStatus() == "Try again");
        packs.installStickerSet();
        response = parse(R"({"@type":"ok"})");
        json_object_set(response,"@extra",json_object_get(request,"@extra")); packs.process(response);
        assert(!packs.stickerSetCanInstall() && packs.sticker_view_installed_);
        json_decref(request);
        packs.openStickers();
        response = parse(R"({"@type":"stickerSet","id":"88","title":"Stale","stickers":[]})");
        json_object_set(response,"@extra",extra); json_decref(extra); packs.process(response);
        assert(!packs.stickerSetOpen() && packs.stickerSetTitle() == "Emoji pack");
        packs.worker_started_=false;
    }
    // Pack icons must be static and independent of the opened sticker grid.
    {
        TelegramClient packs;
        packs.worker_started_=true; packs.ready_=true; packs.current_chat_id_=42;
        packs.sticker_packs_={{0,"Favorites"},{1,"Recent"}}; packs.sticker_set_ids_={0,0};
        auto* sets=parse(R"({"sets":[
            {"id":80000000001,"title":"Video pack","thumbnail":{"format":{"@type":"thumbnailFormatWebm"},"file":{"id":301}},"covers":[{"format":{"@type":"stickerFormatWebm"},"thumbnail":{"format":{"@type":"thumbnailFormatWebp"},"file":{"id":302,"local":{"path":"partial.webp","is_downloading_completed":false}}},"sticker":{"id":303}}]},
            {"id":80000000002,"title":"No cover"},
            {"id":80000000003,"title":"Static","covers":[{"format":{"@type":"stickerFormatWebp"},"sticker":{"id":304}}]},
            {"id":80000000004,"title":"PNG","thumbnail":{"format":{"@type":"thumbnailFormatPng"},"file":{"id":305}}}
        ]})");
        packs.applyStickerResponse(sets,true); json_decref(sets);
        assert(packs.sticker_packs_[2].icon_file_id==302 && packs.sticker_packs_[2].icon_path.empty());
        assert(packs.sticker_packs_[4].icon_file_id==304 && packs.sticker_packs_[5].icon_file_id==305);
        packs.ensureStickerPackIcon(2);
        auto* req=parse(packs.outgoing_.back().text.c_str());
        assert(MediaUploads::number(req,"file_id")==302); json_decref(req);
        size_t requests=packs.outgoing_.size();
        packs.ensureStickerPackIcon(2); assert(packs.outgoing_.size()==requests);
        packs.ensureStickerPackIcon(3);
        assert(packs.outgoing_.size()==requests+1);
        req=parse(packs.outgoing_.back().text.c_str());
        assert(MediaUploads::string(req,"@type")=="getStickerSet" && MediaUploads::number(req,"set_id")==80000000002LL);
        assert(MediaUploads::string(json_object_get(req,"@extra"),"kind")=="sticker_pack_icon");
        json_decref(req);
        requests=packs.outgoing_.size();
        packs.ensureStickerPackIcon(3); assert(packs.outgoing_.size()==requests);
        packs.ensureStickerPackIcon(0); packs.ensureStickerPackIcon(1);
        assert(packs.outgoing_.size()==requests);
        packs.stickers_.push_back({}); packs.stickers_[0].file=999;
        packs.process(parse(R"({"@type":"stickerSet","id":80000000002,"stickers":[{"format":{"@type":"stickerFormatTgs"},"thumbnail":{"format":{"@type":"thumbnailFormatJpeg"},"file":{"id":306}}}],"@extra":{"kind":"sticker_pack_icon","set":80000000002,"session":0}})"));
        assert(packs.sticker_packs_[3].icon_file_id==306 && packs.stickers_[0].file==999);
        packs.process(parse(R"({"@type":"stickerSet","id":80000000002,"stickers":[],"@extra":{"kind":"sticker_pack_icon","set":80000000002,"session":99}})"));
        assert(packs.sticker_packs_[3].icon_file_id==306); // Stale response ignored.
        packs.process(parse(R"({"@type":"updateFile","file":{"id":306,"local":{"is_downloading_completed":false,"path":"partial.jpg"}}})"));
        assert(packs.sticker_packs_[3].icon_path.empty());
        packs.process(parse(R"({"@type":"updateFile","file":{"id":306,"local":{"is_downloading_completed":true,"path":"icon.jpg"}}})"));
        assert(packs.sticker_packs_[3].icon_path=="icon.jpg");
        packs.process(parse(R"({"@type":"updateFile","file":{"id":306,"local":{"is_downloading_completed":false,"path":""}}})"));
        assert(packs.sticker_packs_[3].icon_path.empty());
        requests=packs.outgoing_.size();
        packs.openStickers();
        assert(packs.outgoing_.size()==requests+2);
        req=parse(packs.outgoing_[requests].text.c_str());
        assert(MediaUploads::string(req,"@type")=="getRecentStickers");json_decref(req);
        req=parse(packs.outgoing_[requests+1].text.c_str());
        assert(MediaUploads::string(req,"@type")=="getInstalledStickerSets");json_decref(req);
        requests=packs.outgoing_.size();packs.selectStickerPack(1);
        assert(packs.outgoing_.size()==requests); // Reuse the recent request when selecting its row.
        packs.worker_started_=false;
    }
    {
        TelegramClient reactions;
        reactions.worker_started_=true; reactions.ready_=true; reactions.current_chat_id_=42;
        auto* message=parse(R"({"@type":"message","id":55,"chat_id":42,"content":{"@type":"messageText","text":{"text":"Reactions"}},"interaction_info":{"reactions":{"reactions":[]}}})");
        auto* list=json_object_get(json_object_get(json_object_get(message,"interaction_info"),"reactions"),"reactions");
        for (int i=0;i<64;++i) json_array_append_new(list,json_pack("{s:{s:s,s:I},s:i,s:b}","type","@type","reactionTypeCustomEmoji","custom_emoji_id",(json_int_t)(900000000000LL+i),"total_count",i+1,"is_chosen",i==0));
        auto parsed=reactions.parseMessage(message);
        json_object_set(json_object_get(message,"interaction_info"),"reactions",list);
        auto direct=reactions.parseMessage(message);assert(direct.reactions.size()==64);
        json_decref(message);
        assert(parsed.reactions.size()==64 && parsed.reactions[63].count==64 && parsed.reactions[0].custom_emoji_id==900000000000LL);
        reactions.history_items_={parsed};reactions.rebuildHistory();
        reactions.viewMessage(55);
        auto* viewed=parse(reactions.outgoing_[reactions.outgoing_.size()-2].text.c_str());
        assert(MediaUploads::string(viewed,"@type")=="viewMessages");
        assert(json_integer_value(json_array_get(json_object_get(viewed,"message_ids"),0))==55);json_decref(viewed);
        auto view_requests=reactions.outgoing_.size();reactions.viewMessage(55);
        assert(reactions.outgoing_.size()==view_requests);
        reactions.loadReactions(55);
        reactions.loadReactions(55, true);
        auto* refreshed = parse(reactions.outgoing_.back().text.c_str());
        assert(MediaUploads::string(refreshed,"@type") == "getMessage" && MediaUploads::number(refreshed,"message_id") == 55);
        json_decref(refreshed);
        reactions.process(parse(R"({"@type":"availableReactions","top_reactions":[{"type":{"@type":"reactionTypeCustomEmoji","custom_emoji_id":900000000000},"needs_premium":false}],"recent_reactions":[{"type":{"@type":"reactionTypeCustomEmoji","custom_emoji_id":900000000001},"needs_premium":false}],"popular_reactions":[{"type":{"@type":"reactionTypeCustomEmoji","custom_emoji_id":900000000000},"needs_premium":false},{"type":{"@type":"reactionTypeEmoji","emoji":"X"},"needs_premium":false}],"@extra":{"kind":"reaction_choices","chat":42,"message":55}})"));
        assert(reactions.reaction_choices_.size()==65 && reactions.reactionTopCount()==1);
        assert(reactions.reactionState(0).chosen && reactions.reactionState(0).count == 1);
        assert(!reactions.reactionState(1).chosen && reactions.reactionState(1).count == 2);
        assert(reactions.reactionState(64).count == 64);
        assert(reactions.reactionState(999).count == 0);
        reactions.ensureReactionMedia(0);
        auto* icons=parse(reactions.outgoing_.back().text.c_str());
        assert(MediaUploads::string(icons,"@type")=="getCustomEmojiStickers");
        assert(json_array_size(json_object_get(icons,"custom_emoji_ids"))==31);json_decref(icons);
        auto requests=reactions.outgoing_.size();reactions.ensureReactionMedia(1);
        assert(reactions.outgoing_.size()==requests);
        reactions.process(parse(R"({"@type":"stickers","stickers":[{"full_type":{"custom_emoji_id":900000000000},"format":{"@type":"stickerFormatWebp"},"sticker":{"@type":"file","id":306,"local":{"is_downloading_completed":true,"path":"reaction.webp"}}}],"@extra":{"kind":"reaction_icons","chat":42,"message":55}})"));
        assert(reactions.reactionIcon(0)=="reaction.webp");
        assert(reactions.customReactionIcon(900000000000LL)=="reaction.webp");
        reactions.ensureMessageReactionMedia();
        icons=parse(reactions.outgoing_.back().text.c_str());
        assert(MediaUploads::string(icons,"@type")=="getCustomEmojiStickers");
        // Existing reactions need icons even when absent from the choices.
        assert(json_array_size(json_object_get(icons,"custom_emoji_ids"))==33);json_decref(icons);
        requests=reactions.outgoing_.size();reactions.ensureMessageReactionMedia();
        assert(reactions.outgoing_.size()==requests);
        reactions.process(parse(R"({"@type":"stickers","stickers":[{"full_type":{"custom_emoji_id":900000000063},"format":{"@type":"stickerFormatTgs"},"thumbnail":{"format":{"@type":"thumbnailFormatWebp"},"file":{"id":307,"local":{"is_downloading_completed":false,"path":""}}}}],"@extra":{"kind":"reaction_icons","chat":42,"message":55}})"));
        assert(reactions.customReactionIcon(900000000063LL).empty());
        reactions.ensureMessageReactionMedia();
        auto* download=parse(reactions.outgoing_.back().text.c_str());
        assert(MediaUploads::string(download,"@type")=="downloadFile");
        assert(MediaUploads::number(download,"file_id")==307);json_decref(download);
        reactions.process(parse(R"({"@type":"updateFile","file":{"id":307,"local":{"is_downloading_completed":true,"path":"custom-thumbnail.webp"}}})"));
        assert(reactions.customReactionIcon(900000000063LL)=="custom-thumbnail.webp");
        reactions.process(parse(R"({"@type":"updateFile","file":{"id":306,"local":{"is_downloading_completed":false,"path":""}}})"));
        assert(reactions.reactionIcon(0).empty());
        reactions.toggleReaction(55,reactions.reaction_choices_[0]);
        auto* req=parse(reactions.outgoing_.back().text.c_str());
        assert(MediaUploads::string(req,"@type")=="removeMessageReaction");
        assert(MediaUploads::string(json_object_get(req,"reaction_type"),"@type")=="reactionTypeCustomEmoji");
        assert(MediaUploads::number(json_object_get(req,"reaction_type"),"custom_emoji_id")==900000000000LL);json_decref(req);
        reactions.reaction_pending_=false;
        reactions.toggleReaction(55,reactions.reaction_choices_[1]);
        req=parse(reactions.outgoing_.back().text.c_str());
        assert(MediaUploads::string(req,"@type")=="addMessageReaction");json_decref(req);
        reactions.process(parse(R"({"@type":"updateMessageReactions","chat_id":42,"message_id":55,"reactions":[{"type":{"@type":"reactionTypeEmoji","emoji":"X"},"total_count":123,"is_chosen":true}]})"));
        assert(reactions.messages_[0].reactions.size()==1 && reactions.messages_[0].reactions[0].count==123);
        assert(reactions.reactionState(2).chosen && reactions.reactionState(2).count == 123);
        reactions.process(parse(R"({"@type":"message","id":55,"chat_id":42,"content":{"@type":"messageText","text":{"text":"Refreshed without info"}},"@extra":{"kind":"post_refresh","chat":42,"generation":0}})"));
        assert(reactions.messages_[0].reactions.size()==1 && reactions.messages_[0].reactions[0].count==123);
        assert(reactions.reactionState(2).chosen && reactions.reactionState(2).count == 123);
        reactions.process(parse(R"({"@type":"updateMessageInteractionInfo","chat_id":42,"message_id":55,"interaction_info":{"reactions":{"reactions":[]}}})"));
        assert(reactions.messages_[0].reactions.empty());
        reactions.process(parse(R"({"@type":"availableReactions","top_reactions":[{"type":{"@type":"reactionTypeCustomEmoji","custom_emoji_id":999},"needs_premium":true}],"@extra":{"kind":"reaction_choices","chat":42,"message":55}})"));
        assert(reactions.reaction_choices_.empty() && reactions.reaction_status_.find("Premium")!=std::string::npos);
        assert(reactions.customReactionIcon(900000000063LL)=="custom-thumbnail.webp");
        reactions.loadReactions(56);
        assert(reactions.customReactionIcon(900000000063LL).empty());
        assert(reactions.reactionTopCount()==0);
        auto* preferred=parse(R"({"@type":"availableReactions","top_reactions":[],"recent_reactions":[],"popular_reactions":[{"type":{"@type":"reactionTypeEmoji","emoji":"other"}}],"@extra":{"kind":"reaction_choices","chat":42,"message":56}})");
        auto* top=json_object_get(preferred,"top_reactions");
        for (int i=0;i<10;++i) json_array_append_new(top,json_pack("{s:{s:s,s:s}}","type","@type","reactionTypeEmoji","emoji",("top"+std::to_string(i)).c_str()));
        json_array_append_new(top,json_pack("{s:{s:s,s:s}}","type","@type","reactionTypeEmoji","emoji","top0"));
        reactions.process(preferred);
        assert(reactions.reactionTopCount()==8 && reactions.reactionChoices().size()==11);
        assert(reactions.reactionChoices()[0]=="top0" && reactions.reactionChoices()[8]=="top8");
        reactions.process(parse(R"({"@type":"availableReactions","top_reactions":[{"type":{"@type":"reactionTypeCustomEmoji","custom_emoji_id":999},"needs_premium":true}],"recent_reactions":[{"type":{"@type":"reactionTypeEmoji","emoji":"recent"}}],"popular_reactions":[{"type":{"@type":"reactionTypeEmoji","emoji":"recent"}},{"type":{"@type":"reactionTypeEmoji","emoji":"other"}}],"@extra":{"kind":"reaction_choices","chat":42,"message":56}})"));
        assert(reactions.reactionTopCount()==1 && reactions.reactionChoices().size()==2);
        assert(reactions.reactionChoices()[0]=="recent");
        reactions.worker_started_=false;
    }
    std::puts("Reactions: large custom lists, choices, deduplication, send/remove and updates passed");
    {
        TelegramClient active;
        active.worker_started_=true;active.ready_=true;active.current_chat_id_=42;
        active.history_generation_=3;active.history_pending_=true;
        for (int i=0;i<1000;++i) active.parsed_.push_back(parse(R"({"@type":"updateOption","name":"background","value":{"@type":"optionValueBoolean","value":true}})"));
        active.parsed_.push_back(parse(R"({"@type":"updateNewMessage","message":{"id":100,"chat_id":42,"content":{"@type":"messageText","text":{"text":"Live first"}}}})"));
        auto* first_page=parse(R"({"@type":"messages","messages":[{"id":55,"chat_id":42,"content":{"@type":"messageText","text":{"text":"History"}}}],"@extra":{"kind":"history","chat":42,"generation":3,"first":true}})");
        json_object_set_new(json_object_get(first_page,"@extra"),"requested_at",json_integer(performance::now()-3000));
        json_object_set_new(first_page,"@parsed_at",json_integer(performance::now()-1000));
        active.parsed_.push_back(first_page);
        performance::collector().take(performance::now());
        active.pump(false);
        assert(active.messages_.size()==2 && active.parsed_.size()>=874);
        auto measurements=performance::collector().take(performance::now());
        assert(measurements.get(performance::Metric::FirstHistoryUiWait).count==1);
        assert(measurements.get(performance::Metric::FirstHistoryUiWait).maximum>=1000);
        assert(measurements.get(performance::Metric::FirstHistoryApply).count==1);
        assert(measurements.get(performance::Metric::FirstHistoryReady).maximum>=3000);
        assert(measurements.get(performance::Metric::EventSelect).count>0);
        assert(std::any_of(active.messages_.begin(),active.messages_.end(),[](const MessageItem& m){return m.id==100;}));
        active.chat_open_at_=performance::now()-5000;
        active.chat_present_pending_=true;active.chat_live_present_pending_=true;
        active.chat_live_ready_=false;
        active.messagesPresented();active.messagesPresented();
        measurements=performance::collector().take(performance::now());
        assert(measurements.get(performance::Metric::ChatFirstPresent).count==1);
        assert(measurements.get(performance::Metric::ChatFirstLivePresent).count==0);
        active.chat_live_ready_=true;
        active.messagesPresented();active.messagesPresented();
        measurements=performance::collector().take(performance::now());
        assert(measurements.get(performance::Metric::ChatFirstPresent).count==0);
        assert(measurements.get(performance::Metric::ChatFirstLivePresent).count==1);
        active.sendJson(parse(R"({"@type":"loadChats","chat_list":{"@type":"chatListMain"},"limit":30})"));
        // The displayed list (incl. another folder) passes the interaction gate.
        assert(active.outgoing_.back().backlog_exempt && active.outgoing_.back().priority==5);
        active.requestFile(987654,8);
        assert(!active.outgoing_.back().backlog_exempt);
        auto requests=active.outgoing_.size();
        active.requestFile(987654,8,true);
        assert(active.outgoing_.size()==requests && active.outgoing_.back().backlog_exempt);
        for (auto* root:active.parsed_) json_decref(static_cast<json_t*>(root));
        active.parsed_.clear();
        active.worker_started_=false;
    }
    std::puts("Active chat: production pump applies history and preceding live message before background backlog");
    std::puts("Sticker pack icons: static covers, first-sticker fallback, visible requests, deduplication, stale replies and file updates passed");

    // Startup list: partial metadata must not remove/reorder visible cached rows.
    mkdir("ux0:data", 0777); mkdir("ux0:data/vitagram", 0777);
    mkdir(VITAGRAM_DB_DIR, 0777);
    auto* cached_list = parse(R"({"chats":{"1":{"id":1,"title":"Cached A","positions":[{"list":{"@type":"chatListMain"},"order":200}]},"2":{"id":2,"title":"Deleted while offline","positions":[{"list":{"@type":"chatListMain"},"order":100}]},"3":{"id":3,"title":"Folder cache","positions":[{"list":{"@type":"chatListFolder","chat_folder_id":7},"order":100}]}},"history":{}})");
    assert(!json_dump_file(cached_list, VITAGRAM_DB_DIR "/ui-cache.json", JSON_COMPACT));
    json_decref(cached_list);
    std::remove(VITAGRAM_DB_DIR "/ui-startup.json"); // This fixture replaces the source cache.
    {
        TelegramClient list;
        list.restoreCache();
        assert(list.chats_.size()==2 && list.chats_[0].id==1);
        // Live metadata is shown in place; the cached order stays until the window is confirmed.
        list.process(parse(R"({"@type":"updateNewChat","chat":{"id":1,"title":"Live A","positions":[]}})"));
        list.publishChats();
        assert(list.chats_.size()==2 && list.chats_[0].id==1 && list.chats_[0].title=="Live A");
        list.process(parse(R"({"@type":"updateChatPosition","chat_id":1,"position":{"list":{"@type":"chatListMain"},"order":150}})"));
        list.process(parse(R"({"@type":"ok","@extra":{"kind":"load_main_chats","list_generation":0}})"));
        list.publishChats();
        assert(list.chats_.size()==2 && list.chats_[0].id==1 && list.chats_[1].id==2);
        assert(list.startup_chat_snapshots_.count(0)); // A short first page is not ready.
        list.process(parse(R"({"@type":"updateNewChat","chat":{"id":4,"title":"New top","positions":[{"list":{"@type":"chatListMain"},"order":300}]}})"));
        list.publishChats();
        assert(list.chats_.size()==2 && list.chats_[0].id==1);
        // Connection failure and stale page replies retain the snapshot.
        list.process(parse(R"({"@type":"error","code":500,"message":"offline","@extra":{"kind":"load_main_chats","list_generation":0}})"));
        list.publishChats(); assert(list.chats_[0].id==1);
        list.process(parse(R"({"@type":"ok","@extra":{"kind":"load_main_chats","list_generation":99}})"));
        list.publishChats(); assert(list.chats_[0].id==1);
        list.process(parse(R"({"@type":"ok","@extra":{"kind":"load_main_chats","list_generation":0}})"));
        assert(list.chats_.size()==2 && list.chats_[0].id==4 && list.chats_[1].title=="Live A");
        assert(list.startup_chat_snapshots_.count(7)); // Other folders synchronize separately.
        list.selected_folder_=7;
        list.publishChats(); assert(list.chats_.size()==1);
        list.process(parse(R"({"@type":"error","code":404,"message":"empty","@extra":{"kind":"load_main_chats","list_generation":0}})"));
        assert(list.chats_.empty() && !list.startup_chat_snapshots_.count(7));
        list.process(parse(R"({"@type":"updateChatPosition","chat_id":4,"position":{"list":{"@type":"chatListMain"},"order":0}})"));
        list.selected_folder_=0; list.publishChats();
        assert(list.chats_.size()==1 && list.chats_[0].id==1); // Real removals still work.
    }
    std::puts("Startup chats: stable cache snapshot, atomic replacement, stale/error replies, folders and removals passed");
    {
        // Production pump must adopt a lightweight index while the full disk
        // cache is deliberately blocked on the Storage worker.
        UiCache index;
        auto* chat = parse(R"({"@type":"chat","id":101,"title":"Fast index","positions":[{"list":{"@type":"chatListMain"},"order":100}]})");
        index.remember(chat); json_decref(chat);
        auto* snapshot = index.startupSnapshot(); index.saveStartup(snapshot); json_decref(snapshot);
        storage_entered = false; storage_released = false; full_cache_blocked = true;
        TelegramClient fast; fast.live_list_received_ = true; assert(fast.restoreCacheAsync());
        auto deadline = sceKernelGetSystemTimeWide() + 3000000;
        while (!fast.canBrowse() && sceKernelGetSystemTimeWide() < deadline) {
            fast.pump(); sceKernelDelayThread(1000);
        }
        const bool shown_while_blocked = fast.canBrowse() && !storage_released;
        while (!storage_entered && sceKernelGetSystemTimeWide() < deadline) {
            fast.pump(); sceKernelDelayThread(1000);
        }
        const bool first_screen = shown_while_blocked && storage_entered;
        storage_released = true;
        assert(first_screen && fast.chats_[0].title == "Fast index");
        fast.stopWorker(); full_cache_blocked = false;
        assert(!std::remove(VITAGRAM_DB_DIR "/ui-startup.json"));
    }
    {
        TelegramClient sliced;
        auto* snapshot = parse(R"({"chats":{},"users":{"7":{"id":7,"first_name":"Cached"}}})");
        for (int i = 1; i <= 100; ++i) {
            auto* chat = json_pack("{s:I,s:s,s:[{s:{s:s},s:I}]}", "id", (json_int_t)i, "title", "Cached",
                "positions", "list", "@type", "chatListMain", "order", (json_int_t)(1000-i));
            json_object_set_new(json_object_get(snapshot, "chats"), std::to_string(i).c_str(), chat);
        }
        sliced.beginCacheApply(snapshot); sliced.pump(true);
        assert(sliced.known_chats_.empty()); // Input frame does no cache work.
        sliced.pump(false);
        // Chats are installed in one step (the list never grows row by row); users stay sliced.
        assert(sliced.applying_snapshot_ && sliced.known_chats_.size() == 100 && sliced.chats_.size() == 15);
        sliced.process(parse(R"({"@type":"updateNewChat","chat":{"id":1,"title":"Live wins","positions":[{"list":{"@type":"chatListMain"},"order":2000}]}})"));
        sliced.process(parse(R"({"@type":"updateUser","user":{"id":7,"first_name":"Live name"}})"));
        for (int i = 0; sliced.applying_snapshot_ && i < 200; ++i) sliced.pump(false);
        // An early live chat does not discard the startup rows; it updates its own row.
        assert(!sliced.applying_snapshot_ && sliced.chats_.size() == 15 && sliced.chats_[0].title == "Live wins" &&
               sliced.chats_[1].id == 2 && sliced.startup_chat_snapshots_.count(0));
        assert(sliced.user_names_[7] == "Live name");
        sliced.beginCacheApply(parse(R"({"chats":{"2":{"id":2,"title":"Late"}}})"));
        sliced.clearAccountMemory(); sliced.pump(false);
        assert(!sliced.applying_snapshot_ && sliced.chats_.empty());
    }
    {
        TelegramClient offline;
        offline.current_chat_id_ = 42;
        auto* message = parse(R"({"@type":"updateNewMessage","message":{"id":55,"chat_id":42,"content":{"@type":"messageText","text":{"text":"Offline detail"}}}})");
        offline.ui_cache_.remember(message); json_decref(message);
        offline.requestCachedDetails(); offline.prepareCachedDetails(); offline.applyCachedDetails();
        assert(offline.messages_.size() == 1 && offline.messages_[0].id == 55);
        offline.requestCachedDetails(); offline.prepareCachedDetails();
        offline.current_chat_id_ = 43; offline.history_items_.clear(); offline.messages_.clear();
        offline.applyCachedDetails(); assert(offline.messages_.empty());
        offline.current_chat_id_ = 42; offline.requestCachedDetails(); offline.prepareCachedDetails();
        offline.ready_ = true; offline.applyCachedDetails(); assert(offline.messages_.empty());
        offline.ready_ = false;
        const char* voice_path = VITAGRAM_FILES_DIR "/offline.ogg";
        FILE* voice = std::fopen(voice_path, "wb"); assert(voice); std::fputs("data", voice); std::fclose(voice);
        message = parse(R"({"@type":"updateNewMessage","message":{"id":56,"chat_id":42,"content":{"@type":"messageVoiceNote","voice_note":{"duration":1,"voice":{"@type":"file","id":88,"size":4,"remote":{"unique_id":"offline-voice"},"local":{"path":"ux0:data/vitagram/files/offline.ogg","is_downloading_completed":true}}}}}})");
        offline.ui_cache_.remember(message); json_decref(message);
        offline.requestCachedDetails(); offline.prepareCachedDetails(); offline.applyCachedDetails();
        auto found = std::find_if(offline.messages_.begin(), offline.messages_.end(), [](const MessageItem& m) { return m.id == 56; });
        assert(found != offline.messages_.end() && found->voice_file_id == 0 && found->voice_path == voice_path);
        auto* cached_chat = parse(R"({"@type":"chat","id":42,"title":"Offline avatar","positions":[{"list":{"@type":"chatListMain"},"order":100}],"photo":{"small":{"@type":"file","id":89,"size":4,"remote":{"unique_id":"offline-avatar"},"local":{"path":"ux0:data/vitagram/files/offline.ogg","is_downloading_completed":true}}}})");
        offline.ui_cache_.remember(cached_chat); json_decref(cached_chat);
        offline.applyRestoredCache(offline.ui_cache_.startupSnapshot());
        assert(offline.chats_[0].avatar_path.empty());
        offline.ensureChatMedia(0); offline.ensureChatMedia(0);
        assert(offline.cached_avatar_requests_.size() == 1);
        offline.prepareCachedAvatar(); offline.applyCachedAvatar();
        assert(offline.chats_[0].avatar_path == voice_path && offline.chats_[0].avatar_file_id == 0);
        offline.ready_=true;
        offline.chats_[0].avatar_path.clear(); offline.cached_avatar_seen_.clear();
        offline.ensureChatMedia(0);
        assert(offline.cached_avatar_requests_.size()==1);
        offline.cache_service_at_=0; offline.cache_load_slice_=0;
        assert(TelegramClient::cacheLoadProceed(&offline));
        assert(offline.cached_avatar_requests_.empty());
        offline.applyCachedAvatar();
        assert(offline.chats_[0].avatar_path==voice_path);
        offline.ready_=false;
        auto* items = parse(R"({"stickers":[{"width":64,"height":64,"emoji":"x","format":{"@type":"stickerFormatWebp"},"sticker":{"@type":"file","id":90,"size":4,"remote":{"unique_id":"offline-sticker"},"local":{"path":"ux0:data/vitagram/files/offline.ogg","is_downloading_completed":true}}}]})");
        offline.ui_cache_.saveStickerItems(0, items); json_decref(items);
        offline.requestCachedDetails(0); offline.prepareCachedDetails(); offline.applyCachedDetails();
        assert(offline.stickers_.size() == 1 && offline.stickers_[0].file == 0 && offline.stickers_[0].path == voice_path);
        offline.ready_ = true; assert(!offline.sendSticker(0));
        assert(!std::remove(voice_path));
    }
    {
        // A disk cache above the runtime limits is trimmed once by Storage, not
        // 16 entries per live update on the UI thread.
        // Later scenarios read the cache this one overwrites: keep and restore it.
        std::string previous; bool had_cache = false;
        if (FILE* in = std::fopen(VITAGRAM_DB_DIR "/ui-cache.json", "rb")) {
            had_cache = true; char chunk[4096]; size_t n;
            while ((n = std::fread(chunk, 1, sizeof chunk, in)) > 0) previous.append(chunk, n);
            std::fclose(in);
        }
        auto* big = json_pack("{s:{},s:{},s:{}}", "chats", "history", "users");
        for (int i = 1; i <= 1100; ++i) {
            std::string id = std::to_string(i);
            json_object_set_new(json_object_get(big, "users"), id.c_str(), json_pack("{s:i,s:s}", "id", i, "first_name", "u"));
        }
        assert(!json_dump_file(big, VITAGRAM_DB_DIR "/ui-cache.json", JSON_COMPACT)); json_decref(big);
        UiCache trimmed; trimmed.load();
        auto* users = trimmed.users();
        assert(json_object_size(users) == 1024 && !json_object_get(users, "1") && json_object_get(users, "1100"));
        json_decref(users);
        if (had_cache) {
            FILE* out = std::fopen(VITAGRAM_DB_DIR "/ui-cache.json", "wb");
            assert(out && std::fwrite(previous.data(), 1, previous.size(), out) == previous.size());
            std::fclose(out);
        } else assert(!std::remove(VITAGRAM_DB_DIR "/ui-cache.json"));
    }
    std::puts("Startup slices: bounded adoption, live data precedence, blocked full cache and on-demand offline history passed");
    {
        // Offline startup uses the real background worker too; UI owns adoption.
        TelegramClient async;
        assert(async.restoreCacheAsync());
        assert(!async.restoreCacheAsync()); // Never launch a second loader.
        auto deadline = sceKernelGetSystemTimeWide() + 3000000;
        while (!async.canBrowse() && sceKernelGetSystemTimeWide() < deadline) {
            async.pump(); sceKernelDelayThread(1000);
        }
        assert(async.canBrowse());
        async.stopWorker(); assert(!async.restored_snapshot_);
    }
    {
        TelegramClient late;
        late.process(parse(R"({"@type":"updateNewChat","chat":{"id":77,"title":"Live","positions":[{"list":{"@type":"chatListMain"},"order":500}]}})"));
        late.process(parse(R"({"@type":"updateUser","user":{"id":7,"first_name":"Live user"}})"));
        late.process(parse(R"({"@type":"updateChatFolders","chat_folders":[]})"));
        late.applyRestoredCache(parse(R"({"chats":{"1":{"id":1,"title":"Old","positions":[{"list":{"@type":"chatListMain"},"order":900}]}},"users":{"7":{"id":7,"first_name":"Old user"}},"folders":{"chat_folders":[{"id":5,"title":"Old folder"}]}})"));
        // Chats TDLib announced before loadChats are not a list yet: the startup rows stay
        // until Telegram confirms the window, then the live list replaces them.
        assert(late.chats_.size() == 1 && late.chats_[0].title == "Old");
        late.process(parse(R"({"@type":"ok","@extra":{"kind":"load_main_chats","list_generation":0}})"));
        assert(late.chats_.size() == 1 && late.chats_[0].title == "Live");
        // A list confirmed in this session is never replaced by a late index.
        late.applyRestoredCache(parse(R"({"chats":{"2":{"id":2,"title":"Older","positions":[{"list":{"@type":"chatListMain"},"order":950}]}},"users":{}})"));
        assert(late.chats_.size() == 1 && late.chats_[0].title == "Live");
        // Live data wins over the restored cache: the cached folder (id 5) does not come back. Only the
        // built-in "All chats" and "Archive" tabs, which the client always adds, may be listed.
        assert(late.user_names_[7] == "Live user");
        for (const auto& folder : late.folders_) assert(folder.id == 0 || folder.id == kArchiveFolderId);
        late.clearAccountMemory();
        late.applyRestoredCache(parse(R"({"chats":{"1":{"id":1,"title":"Old"}},"users":{}})"));
        assert(late.known_chats_.empty());
    }
    std::puts("Async cache: offline startup, single worker, UI adoption, live data priority and logout rejection passed");
    {
        // The list Telegram confirmed is what the next launch shows first: unread state is kept
        // and each list keeps only its top rows.
        TelegramClient saved;
        saved.ui_cache_.clear(); saved.full_cache_loaded_ = true;
        for (int i = 1; i <= 130; ++i) {
            ChatItem chat; chat.id = i; chat.title = "Chat " + std::to_string(i); chat.user_id = 1000 + i;
            chat.list_orders[0] = 10000 - i;
            if (i == 130) chat.list_orders[7] = 50; // Folder rows survive the main-list cut.
            chat.unread_count = i == 2 ? 3 : 0; chat.marked_unread = i == 3; chat.last_message_id = 500 + i;
            saved.known_chats_.push_back(chat);
        }
        saved.queueStartupList(); assert(!saved.pending_startup_list_); // Not confirmed by Telegram yet.
        saved.chats_last_updated_[0] = 1; saved.startup_list_dirty_ = true;
        saved.queueStartupList(); assert(saved.pending_startup_list_ && !saved.startup_list_dirty_);
        saved.writeStartupList(true); assert(!saved.pending_startup_list_);
        UiCache reader; auto* index = reader.loadStartup(); assert(index);
        auto* chats = json_object_get(index, "chats");
        assert(json_object_size(chats) == 101 && json_object_get(chats, "130") && !json_object_get(chats, "120"));
        TelegramClient next; next.applyRestoredCache(index);
        assert(next.chats_.size() == 15 && next.chats_[0].id == 1 && next.chats_[1].unread_count == 3 && next.chats_[2].marked_unread);
        assert(next.chats_[0].user_id == 1001 && next.startup_chat_snapshots_[7].size() == 1);
        assert(!std::remove(VITAGRAM_DB_DIR "/ui-startup.json"));
    }
    std::puts("Startup index: confirmed list saved with unread state, per-list limit and folder rows passed");
    {
        // Avatars: drawn from the index at once and kept while TDLib re-reports or re-downloads them.
        TelegramClient avatars;
        avatars.applyRestoredCache(parse(R"({"chats":{"5":{"id":5,"title":"A","@avatar_path":"ux0:old.jpg","positions":[{"list":{"@type":"chatListMain"},"order":10}]}},"users":{}})"));
        assert(avatars.chats_.size() == 1 && avatars.chats_[0].avatar_path == "ux0:old.jpg");
        avatars.process(parse(R"({"@type":"updateNewChat","chat":{"id":5,"title":"A","positions":[],"photo":{"small":{"@type":"file","id":91,"local":{"path":"","is_downloading_completed":false}}}}})"));
        avatars.publishChats();
        assert(avatars.chats_[0].avatar_path == "ux0:old.jpg" && avatars.chats_[0].avatar_stale && avatars.chats_[0].avatar_file_id == 91);
        avatars.process(parse(R"({"@type":"updateFile","file":{"id":91,"size":10,"local":{"path":"","is_downloading_completed":false}}})"));
        avatars.publishChats(); assert(avatars.chats_[0].avatar_path == "ux0:old.jpg");
        avatars.process(parse(R"({"@type":"updateFile","file":{"id":91,"size":10,"local":{"path":"ux0:new.jpg","is_downloading_completed":true}}})"));
        avatars.publishChats(); assert(avatars.chats_[0].avatar_path == "ux0:new.jpg" && !avatars.chats_[0].avatar_stale);
        avatars.process(parse(R"({"@type":"updateNewChat","chat":{"id":5,"title":"A","positions":[]}})"));
        avatars.publishChats(); assert(avatars.chats_[0].avatar_path.empty()); // Removed photo.
    }
    std::puts("Startup avatars: shown from the index, kept through live metadata and downloads, cleared on removal passed");
    {
        // Server read position: parsed, updated by read-inbox updates without touching the open history, and kept in the cache.
        TelegramClient reader;
        reader.applyRestoredCache(parse(R"({"chats":{"8":{"id":8,"title":"R","positions":[{"list":{"@type":"chatListMain"},"order":10}],"unread_count":4,"last_read_inbox_message_id":120,"last_message":{"id":200}}},"users":{}})"));
        reader.publishChats();
        assert(reader.chats_.size() == 1 && reader.chats_[0].last_read_inbox == 120);
        reader.process(parse(R"({"@type":"updateNewChat","chat":{"id":8,"title":"R","positions":[{"list":{"@type":"chatListMain"},"order":10}],"unread_count":4,"last_read_inbox_message_id":150,"last_message":{"id":200}}})"));
        assert(reader.known_chats_.size() == 1 && reader.known_chats_[0].last_read_inbox == 150);
        reader.current_chat_id_ = 8; reader.history_items_.push_back(MessageItem{}); reader.history_items_.back().id = 999;
        reader.process(parse(R"({"@type":"updateChatReadInbox","chat_id":8,"last_read_inbox_message_id":180,"unread_count":2})"));
        assert(reader.known_chats_[0].last_read_inbox == 180 && reader.known_chats_[0].unread_count == 2);
        assert(reader.history_items_.size() == 1 && reader.history_items_[0].id == 999);
        reader.process(parse(R"({"@type":"updateChatIsMarkedAsUnread","chat_id":8,"is_marked_as_unread":true})"));
        assert(reader.known_chats_[0].last_read_inbox == 180);
        reader.current_chat_id_ = 0; reader.history_items_.clear();
        TelegramClient cached; cached.applyRestoredCache(parse(R"({"chats":{"8":{"id":8,"title":"R","positions":[{"list":{"@type":"chatListMain"},"order":10}],"unread_count":2,"last_read_inbox_message_id":180,"last_message":{"id":300}}},"users":{}})"));
        cached.publishChats();
        assert(cached.chats_.size() == 1 && cached.chats_[0].last_read_inbox == 180);
        // Open at the read position: the anchor stays set until the boundary is loaded, then the first message after it is jumped to.
        TelegramClient open;
        open.ready_ = true;
        open.known_chats_.push_back(cached.chats_[0]);
        open.openChat(8, i18n::Text{});
        assert(!open.historyAnchorPending()); // Default is latest.
        open.setChatOpenPosition(1);
        open.openChat(8, i18n::Text{});
        assert(open.history_anchor_ == 180 && open.historyAnchorSeeking());
        open.history_items_.push_back(MessageItem{}); open.history_items_.back().id = -1;
        assert(open.historyAnchorSeeking()); // A pending outgoing message is not a server read boundary.
        open.history_items_.clear();
        for (int64_t id : {170, 180, 190, 200}) { MessageItem item; item.id = id; open.history_items_.push_back(item); }
        open.rebuildHistory(false);
        open.resolveHistoryAnchor();
        assert(open.history_anchor_ == 0 && open.message_jump_ == 180);
        assert(open.takeMessageJump() == 180 && !open.messageJumpOpensPost());
        open.ui_cache_.setLocalReadPosition(8, 170);
        open.setChatOpenPosition(2); open.openChat(8, "Device");
        assert(open.history_anchor_ == 170);
        open.viewMessage(200); // Loading must never mark the newest message as read.
        assert(open.ui_cache_.localReadPosition(8) == 170);
        open.messagesPresented(200); assert(open.ui_cache_.localReadPosition(8) == 170);
        open.history_anchor_ = 0;
        open.history_items_ = {MessageItem{}}; open.history_items_.front().id = 190; open.rebuildHistory(false);
        open.messagesPresented(190); assert(open.ui_cache_.localReadPosition(8) == 190);
        open.viewMessage(190); open.viewMessage(170);
        assert(open.ui_cache_.localReadPosition(8) == 190);
        open.process(parse(R"({"@type":"updateChatReadInbox","chat_id":8,"last_read_inbox_message_id":200,"unread_count":0})"));
        assert(open.ui_cache_.localReadPosition(8) == 190); // Other clients cannot move the local marker.
        open.openChat(8, "Device again"); assert(open.history_anchor_ == 190);
        open.ui_cache_.setLocalReadPosition(9, 120);
        auto* saved = open.ui_cache_.startupSnapshot();
        assert(json_integer_value(json_object_get(json_object_get(saved, "local_read"), "8")) == 190);
        open.ui_cache_.saveStartup(saved); json_decref(saved);
        UiCache restored;
        saved = restored.loadStartup(); assert(saved); json_decref(saved);
        assert(restored.localReadPosition(8) == 190 && restored.localReadPosition(9) == 120);
        restored.setLocalReadPosition(8, 220); restored.load();
        assert(restored.localReadPosition(8) == 220); // An early read wins over disk restore.
        restored.clear(); assert(restored.localReadPosition(8) == 0);

        // Reach the memory cap without the target: keep the modal pending until acknowledgement.
        open.setChatOpenPosition(1); open.openChat(8, "Distant");
        open.history_anchor_ = 10; open.history_cached_ = false; open.history_items_.clear();
        for (int64_t id = 100; id < 400; ++id) { MessageItem item; item.id = id; open.history_items_.push_back(item); }
        open.rebuildHistory(false); open.resolveHistoryAnchor();
        assert(open.historyAnchorPending() && open.historyAnchorFar() && !open.takeMessageJump());
        open.worker_started_ = true; open.outgoing_.clear(); open.acceptHistoryAnchorFar();
        assert(open.historyAnchorPending() && !open.historyAnchorFar() && open.messages().empty());
        auto respond_window = [&](const char* payload) {
            auto* req = parse(open.outgoing_.back().text.c_str());
            assert(!std::strcmp(json_string_value(json_object_get(req, "@type")), "getChatHistory"));
            auto* response = parse(payload); json_object_set(response, "@extra", json_object_get(req, "@extra"));
            json_decref(req); open.outgoing_.clear(); open.process(response);
        };
        respond_window(R"({"@type":"messages","messages":[{"id":20,"chat_id":8},{"id":10,"chat_id":8},{"id":5,"chat_id":8}]})");
        assert(!open.historyAnchorPending() && open.messages().size() == 3 && open.takeMessageJump() == 10);
        open.process(parse(R"({"@type":"updateNewMessage","message":{"id":1000,"chat_id":8}})"));
        assert(open.messages().size() == 3 && open.messages().back().id == 20);
        open.loadNewer();
        respond_window(R"({"@type":"messages","messages":[{"id":40,"chat_id":8},{"id":30,"chat_id":8},{"id":20,"chat_id":8}]})");
        assert(open.messages().size() == 3 && open.takeMessageJump() == 30 && !open.messageJumpOpensPost());
        open.loadOlder();
        respond_window(R"({"@type":"messages","messages":[{"id":20,"chat_id":8},{"id":10,"chat_id":8},{"id":5,"chat_id":8}]})");
        assert(open.messages().size() == 3 && open.takeMessageJump() == 10);
        open.loadNewer(true);
        respond_window(R"({"@type":"messages","messages":[{"id":40,"chat_id":8},{"id":30,"chat_id":8},{"id":20,"chat_id":8}]})");
        assert(open.takeMessageJump() == 30 && open.messageJumpOpensPost());
        open.history_anchor_ = 1; open.requestHistoryWindow(1);
        open.history_deadline_ = sceKernelGetSystemTimeWide() - 1;
        open.pump();
        assert(!open.history_pending_ && open.historyAnchorPending() && !open.historyError().empty());
        open.retryHistoryOpening();
        respond_window(R"({"@type":"error","code":400,"message":"Unavailable"})");
        assert(open.historyAnchorPending() && !open.historyError().empty() && !open.history_pending_);
        open.retryHistoryOpening();
        respond_window(R"({"@type":"messages","messages":[{"id":2,"chat_id":8}]})");
        assert(!open.historyAnchorPending() && open.takeMessageJump() == 2); // Deleted target: nearest remaining message.
        open.history_anchor_ = 10; open.requestHistoryWindow(10);
        auto* stale_request = parse(open.outgoing_.back().text.c_str());
        open.leaveChat(); open.openChat(9, "Other");
        auto* stale_response = parse(R"({"@type":"messages","messages":[{"id":10,"chat_id":8}]})");
        json_object_set(stale_response, "@extra", json_object_get(stale_request, "@extra")); json_decref(stale_request);
        open.process(stale_response); assert(open.messages().empty());
        open.worker_started_ = false;
        open.topics_ = {{7, "Topic", 2, false, 150}};
        open.setChatOpenPosition(1); open.openTopic(0); assert(open.history_anchor_ == 150);
        open.ui_cache_.setLocalReadPosition(9, 160, 7);
        open.setChatOpenPosition(2); open.openTopic(0); assert(open.history_anchor_ == 160);
        assert(open.ui_cache_.localReadPosition(9) == 120); // Topics do not advance the parent chat.
        open.leaveChat(); open.pending_thread_ = 80; open.pending_read_anchor_ = 120;
        open.setChatOpenPosition(1); open.openChat(9, "Comments"); assert(open.history_anchor_ == 120);
        open.ui_cache_.setLocalReadPosition(9, 140, -80);
        open.pending_thread_ = 80; open.setChatOpenPosition(2); open.openChat(9, "Comments");
        assert(open.history_anchor_ == 140);
        open.history_items_ = {MessageItem{}}; open.history_items_.front().id = 130; open.rebuildHistory(false);
        assert(open.openPinnedMessage(130) && !open.historyAnchorPending());
    }
    std::puts("Chat read position: parsed, live-updated, cached and used as the open anchor passed");

    // The synchronized mode must never move behind this device's own read progress.
    for (const auto& positions : {std::pair<int64_t, int64_t>{100, 200}, {200, 100}, {150, 150}, {0, 100}, {100, 0}, {0, 0}}) {
        const int64_t server = positions.first, local = positions.second;
        TelegramClient client;
        ChatItem chat; chat.id = 42; chat.last_read_inbox = server; client.known_chats_ = {chat};
        client.ui_cache_.setLocalReadPosition(42, local);
        client.ui_cache_.setLocalReadPosition(42, local, 7);
        client.ui_cache_.setLocalReadPosition(42, local, -80);
        for (int mode : {0, 1, 2}) {
            const int64_t expected = mode == 0 ? 0 : mode == 1 ? std::max(server, local) : local;
            client.setChatOpenPosition(mode);
            client.openChat(42, "Chat"); assert(client.history_anchor_ == expected);
            client.topics_ = {{7, "Topic", 0, false, server}};
            client.openTopic(0); assert(client.history_anchor_ == expected);
            client.pending_thread_ = 80; client.pending_read_anchor_ = server;
            client.openChat(42, "Comments"); assert(client.history_anchor_ == expected);
        }
    }
    std::puts("Chat opening priority: newer device/server position, equal/missing markers and all three modes across chats/topics/comments passed");

    // GIF requests through the real client handler, not just the catalog parser.
    {
        TelegramClient client;
        client.worker_started_ = true; client.ready_ = true; client.current_chat_id_ = 42;
        ChatItem chat; chat.id = 42; chat.user_id = 7; client.known_chats_ = {chat};
        auto response = [&](const char* text) {
            assert(!client.outgoing_.empty());
            auto* request = parse(client.outgoing_.back().text.c_str());
            auto* root = parse(text);
            json_object_set(root, "@extra", json_object_get(request, "@extra"));
            json_decref(request); client.outgoing_.clear(); client.process(root);
        };
        client.searchGifs();
        assert(client.gifs_.pending && client.gifs_.deadline);
        response(R"({"@type":"animations","animations":[{"animation":{"id":51},"minithumbnail":{"data":"/9j/"},"thumbnail":{"format":{"@type":"thumbnailFormatMpeg4"},"file":{"id":52}}}]})");
        assert(!client.gifs_.pending && client.gifs_.items.size() == 1);
        assert(client.gifs_.items[0].fallback == "inline-jpeg:/9j/");
        assert(client.gifs_.items[0].preview == 52 && client.gifs_.items[0].video_preview);
        client.ensureGif(0);
        assert(!client.outgoing_.empty());
        auto* preview_request = parse(client.outgoing_.back().text.c_str());
        assert(!std::strcmp(json_string_value(json_object_get(preview_request, "@type")), "downloadFile"));
        assert(json_integer_value(json_object_get(preview_request, "file_id")) == 52);
        json_decref(preview_request); client.outgoing_.clear();
        client.process(parse(R"({"@type":"updateFile","file":{"id":52,"local":{"path":"thumb.mp4","is_downloading_completed":true}}})"));
        assert(client.gifs_.items[0].path == "video-thumb:thumb.mp4");
        assert(!client.pending_files_.count(51)); // browsing downloads only thumbnails
        client.ensureGif(0, true);
        auto* full_request = parse(client.outgoing_.back().text.c_str());
        assert(MediaUploads::string(full_request, "@type") == "downloadFile");
        assert(MediaUploads::number(full_request, "file_id") == 51);
        json_decref(full_request);
        const auto request_count = client.outgoing_.size();
        client.ensureGif(0, true);
        assert(client.outgoing_.size() == request_count);
        client.process(parse(R"({"@type":"updateFile","file":{"id":51,"local":{"path":"animation.mp4","is_downloading_completed":false}}})"));
        assert(client.gifs_.items[0].full_path.empty());
        client.process(parse(R"({"@type":"updateFile","file":{"id":51,"local":{"path":"animation.mp4","is_downloading_completed":true}}})"));
        assert(client.gifs_.items[0].full_path == "animation.mp4");
        assert(client.gifs_.items[0].path == "video-thumb:thumb.mp4");
        client.outgoing_.clear();
        client.searchGifs("cats");
        response(R"({"@type":"optionValueString","value":"gif"})");
        assert(client.gifs_.pending);
        response(R"({"@type":"chat","id":100,"type":{"@type":"chatTypePrivate","user_id":100}})");
        response(R"({"@type":"inlineQueryResults","inline_query_id":"9007199254740993","results":[{"@type":"inlineQueryResultAnimation","id":"cat","animation":{"animation":{"id":61}}}],"next_offset":""})");
        assert(!client.gifs_.pending && client.gifs_.items.size() == 1);
        assert(client.setCompositionReply(55));
        assert(client.sendGif(0));
        auto* send = parse(client.outgoing_.back().text.c_str());
        assert(!std::strcmp(json_string_value(json_object_get(send, "query_id")), "9007199254740993"));
        assert(MediaUploads::number(json_object_get(send, "reply_to"), "message_id") == 55 && !client.replyTarget());
        json_decref(send); client.outgoing_.clear();
        client.searchGifs(); client.gifs_.deadline = 1;
        const auto generation = client.gifs_.generation;
        auto* late = parse(R"({"@type":"animations","animations":[]})");
        json_object_set_new(late, "@extra", client.gifs_.extra("gif_saved"));
        client.worker_started_ = false; client.outgoing_.clear(); client.pump();
        assert(!client.gifs_.pending && client.gifs_.generation == generation + 1);
        const auto timeout = client.gif_status_;
        client.process(late);
        assert(client.gif_status_ == timeout);
    }
    std::puts("GIF client: saved results, bot resolution, string query IDs, sending and timeout recovery passed");

    {
        TelegramClient streaming;
        streaming.current_chat_id_ = 42;
        streaming.process(parse(R"({"@type":"updateNewMessage","message":{"id":901,"chat_id":42,"content":{"@type":"messageVideo","video":{"width":640,"height":360,"duration":10,"video":{"@type":"file","id":901,"size":1048576,"local":{"path":"partial.mp4","is_downloading_completed":false,"download_offset":0,"downloaded_prefix_size":131072,"downloaded_size":786432}}}}}})"));
        assert(streaming.messages_.size() == 1);
        auto stream = streaming.messages_[0].animation_stream;
        assert(stream && !stream->ready() && streaming.messages_[0].animation_path.empty());
        streaming.process(parse(R"({"@type":"updateFile","file":{"@type":"file","id":901,"size":1048576,"local":{"path":"partial.mp4","is_downloading_completed":false,"download_offset":65536,"downloaded_prefix_size":786432,"downloaded_size":851968}}})"));
        assert(!stream->ready()); // Total bytes and a nonzero-offset prefix cannot start playback.
        streaming.process(parse(R"({"@type":"updateFile","file":{"@type":"file","id":901,"size":1048576,"local":{"path":"partial.mp4","is_downloading_completed":false,"download_offset":0,"downloaded_prefix_size":262144,"downloaded_size":851968}}})"));
        assert(stream->ready() && stream->snapshot().prefix == 262144);
        auto* old_message = parse(R"({"content":{"@type":"messageVideo","video":{"video":{"@type":"file","id":901,"size":1048576,"local":{"path":"partial.mp4","is_downloading_completed":false,"downloaded_prefix_size":0}}}}})");
        auto reparsed = streaming.parseMessage(old_message); json_decref(old_message);
        assert(reparsed.animation_stream == stream && stream->snapshot().prefix == 262144);
        const auto source = stream->source;
        // The actual TDLib path must be published before cache validation hides it.
        streaming.process(parse(R"({"@type":"updateFile","file":{"@type":"file","id":901,"size":1048576,"remote":{"unique_id":"progressive-video"},"local":{"path":"final.mp4","is_downloading_completed":true,"downloaded_prefix_size":1048576,"downloaded_size":1048576}}})"));
        assert(stream->snapshot().complete && stream->snapshot().path == "final.mp4");
        assert(stream->source == source && streaming.messages_[0].animation_stream == stream);
        assert(streaming.messages_[0].animation_path.empty()); // Async validation still pending.
        streaming.clearAccountMemory();
        assert(streaming.media_streams_.empty());
    }
    std::puts("Progressive playback: live prefix bounds, offset gaps, stale message metadata, completion before cache validation and stable source passed");

    // Real disk cleanup, asynchronous lifecycle and crash recovery, using only
    // the test runner's isolated temporary working directory.
    mkdir("ux0:data", 0777); mkdir("ux0:data/vitagram", 0777);
    mkdir(VITAGRAM_DB_DIR, 0777); mkdir(VITAGRAM_FILES_DIR, 0777);
    auto write = [](const char* path) {
        FILE* f = std::fopen(path, "wb"); assert(f); std::fputs("private", f); assert(!std::fclose(f));
    };
    write("ux0:data/vitagram/settings.json");
    write("ux0:data/vitagram/voice-message-123.ogg");
    write("ux0:data/vitagram/media-upload.log");
    write("ux0:data/vitagram/animation-error.log");
    write("ux0:data/vitagram/gif.log");
    write("ux0:data/vitagram/gif.previous.log");
    write(VITAGRAM_DB_DIR "/ui-cache.json.tmp");
    write(VITAGRAM_FILES_DIR "/photo.jpg");
    write("outside-account.txt");
    assert(!symlink("../../../outside-account.txt", VITAGRAM_FILES_DIR "/external-link"));
    {
        TelegramClient account;
        account.restoreCache();
        assert(account.start());
        account.ready_ = true;
        account.known_chats_ = {group};
        account.stickers_.push_back({});
        account.read_outbox_[42] = 20;
        account.process(parse(R"({"@type":"updateAuthorizationState","authorization_state":{"@type":"authorizationStateLoggingOut"}})"));
        assert(account_cleanup::pending());
        assert(!account.canBrowse() && account.stickers_.empty() && account.read_outbox_.empty());
        account.process(parse(R"({"@type":"updateNewChat","chat":{"id":42,"title":"late private chat"}})"));
        assert(account.known_chats_.empty());
        assert(json_object_size(json_object_get(account.ui_cache_.data_, "chats")) == 0);
        account.process(parse(R"({"@type":"updateAuthorizationState","authorization_state":{"@type":"authorizationStateClosed"}})"));
        account.pump();
        while (account.cleanup_result_ == 0) sceKernelDelayThread(1000);
        account.pump();
        assert(account.cleanup_result_ == 1);
        assert(!account.worker_started_ && !account.cache_worker_started_);
    }
    struct stat info{};
    assert(stat(VITAGRAM_DB_DIR, &info) && stat(VITAGRAM_FILES_DIR, &info));
    assert(stat("ux0:data/vitagram/voice-message-123.ogg", &info));
    assert(stat("ux0:data/vitagram/media-upload.log", &info));
    assert(stat("ux0:data/vitagram/animation-error.log", &info));
    assert(stat("ux0:data/vitagram/gif.log", &info));
    assert(stat("ux0:data/vitagram/gif.previous.log", &info));
    assert(!stat("ux0:data/vitagram/settings.json", &info));
    assert(!stat("outside-account.txt", &info)); // Never follow symlinks.
    assert(!account_cleanup::pending());
    // A deletion failure keeps the recovery marker for the next launch.
    if (geteuid() != 0) {
        mkdir(VITAGRAM_FILES_DIR, 0777);
        write(VITAGRAM_FILES_DIR "/locked");
        assert(account_cleanup::mark());
        assert(!chmod(VITAGRAM_FILES_DIR, 0500));
        assert(!account_cleanup::purge() && account_cleanup::pending());
        assert(!chmod(VITAGRAM_FILES_DIR, 0700));
        assert(account_cleanup::purge() && !account_cleanup::pending());
    }
    // A shutdown interrupted after logout began must not restore old chats.
    mkdir(VITAGRAM_DB_DIR, 0777);
    write(VITAGRAM_DB_DIR "/stale-session");
    assert(account_cleanup::mark());
    {
        TelegramClient recovery;
        recovery.restoreCache();
        assert(!recovery.canBrowse());
        assert(recovery.start(true));
        assert(!account_cleanup::pending());
        assert(stat(VITAGRAM_DB_DIR "/stale-session", &info));
        recovery.restoreCache();
        assert(!recovery.canBrowse());
        recovery.stopWorker();
    }
    std::puts("Logout: memory, late responses, disk cleanup, settings preservation, symlinks and restart recovery passed");
    std::puts("Transfers: production workers, backpressure, progress, parallel sends and destinations passed");
}
