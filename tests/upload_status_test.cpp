#include "test_language.h"
#include "media_uploads.h"
#include "td_event_queue.h"
#include <cassert>
#include <cstdio>

static json_t* parse(const char* text) {
    json_error_t error{}; auto* value = json_loads(text, 0, &error); assert(value); return value;
}
static void event(MediaUploads& uploads, const char* text, int64_t time) {
    auto* value = parse(text); uploads.process(value, time); json_decref(value);
}
int main() {
    use_test_language();
    MediaUploads speed;
    json_decref(speed.begin("voice.ogg", 5000, false, 42, 1000000));
    event(speed, R"({"@type":"updateFile","file":{"id":8,"size":5000,"local":{"path":"voice.ogg"},"remote":{"uploaded_size":1000,"is_uploading_active":true}}})", 2000000);
    assert(speed.items()[0].speed == 0);
    event(speed, R"({"@type":"updateFile","file":{"id":8,"remote":{"uploaded_size":3000,"is_uploading_active":true}}})", 3000000);
    assert(speed.items()[0].speed == 2000);
    // An older queued snapshot must not turn a later observation into a retry.
    event(speed, R"({"@type":"updateFile","file":{"id":8,"remote":{"uploaded_size":2000,"is_uploading_active":true}}})", 2500000);
    assert(speed.items()[0].done == 3000 && speed.items()[0].speed == 2000);
    assert(speed.summary(9000000).find("/с") == std::string::npos);
    event(speed, R"({"@type":"message","id":100,"chat_id":42,"sending_state":{"@type":"messageSendingStatePending"},"@extra":{"kind":"media_upload","token":1},"content":{"voice_note":{"voice":{"id":8,"size":5000,"remote":{"is_uploading_completed":true}}}}})", 10000000);
    assert(speed.items()[0].uploaded && !speed.items()[0].terminal && speed.items()[0].speed == 0);
    assert(speed.summary(10000000).find("подтверждение Telegram") != std::string::npos);
    event(speed, R"({"@type":"updateMessageSendSucceeded","old_message_id":100,"message":{"id":900,"chat_id":42}})", 11000000);
    assert(speed.items()[0].terminal && speed.items()[0].done == 5000);
    assert(speed.lines(14000000).empty());
    assert(!speed.lines(14000000, true).empty());

    MediaUploads sent;
    MediaUploads awaiting;
    json_decref(awaiting.begin("pending.ogg", 100, false, 42, 1000000));
    event(awaiting, R"({"@type":"updateFile","file":{"id":8,"size":100,"local":{"path":"pending.ogg"},"remote":{"uploaded_size":100,"is_uploading_active":true,"is_uploading_completed":false}}})", 2000000);
    assert(!awaiting.items()[0].terminal && !awaiting.items()[0].uploaded);
    const auto waiting = awaiting.summary(32000000);
    assert(waiting.find("100%") != std::string::npos);
    assert(waiting.find("ожидание завершения загрузки Telegram") != std::string::npos);
    assert(waiting.find("нет прогресса") == std::string::npos && waiting.find("/с") == std::string::npos);
    json_decref(sent.begin("ready.ogg", 100, false, 42, 1000000));
    event(sent, R"({"@type":"message","id":100,"chat_id":42,"@extra":{"kind":"media_upload","token":1}})", 2000000);
    assert(sent.items()[0].terminal && sent.items()[0].error.empty());
    MediaUploads failed;
    json_decref(failed.begin("failed.ogg", 100, false, 42, 1000000));
    event(failed, R"({"@type":"message","id":100,"chat_id":42,"sending_state":{"@type":"messageSendingStateFailed","error":{"message":"FAILED"}},"@extra":{"kind":"media_upload","token":1}})", 2000000);
    assert(failed.items()[0].terminal && failed.items()[0].error == "FAILED");

    MediaUploads reordered;
    json_decref(reordered.begin("early.ogg", 100, false, 42, 1000000));
    event(reordered, R"({"@type":"updateMessageSendSucceeded","old_message_id":100,"message":{"id":900,"chat_id":42}})", 2000000);
    event(reordered, R"({"@type":"message","id":100,"chat_id":42,"sending_state":{"@type":"messageSendingStatePending"},"@extra":{"kind":"media_upload","token":1}})", 3000000);
    assert(reordered.items()[0].terminal && reordered.items()[0].ended == 2000000);
    event(reordered, R"({"@type":"updateFile","file":{"id":8,"local":{"path":"early.ogg"},"remote":{"uploaded_size":0,"is_uploading_active":true}}})", 4000000);
    assert(reordered.items()[0].terminal && reordered.items()[0].done == 100);

    MediaUploads sticker;
    json_decref(sticker.beginSticker(8, 42, 1000000));
    event(sticker, R"({"@type":"updateNewMessage","message":{"id":100,"chat_id":42,"sending_state":{"@type":"messageSendingStatePending","sending_id":1},"content":{"sticker":{"sticker":{"id":8}}}}})", 2000000);
    assert(sticker.items()[0].message == 100);
    assert(sticker.summary(2000000).find("Стикер") != std::string::npos);
    assert(sticker.summary(2000000).find("%") == std::string::npos);
    event(sticker, R"({"@type":"updateMessageSendSucceeded","old_message_id":100,"message":{"id":900,"chat_id":42}})", 3000000);
    assert(sticker.summary(3000000) == "Стикер: отправлено");
    sticker.dismissCompleted(); assert(sticker.lines(3000000).empty());
    assert(!sticker.lines(3000000, true).empty());
    MediaUploads latest;
    json_decref(latest.begin("old.ogg", 100, false, 42, 1000000));
    json_decref(latest.begin("new.mp4", 100, true, 42, 2000000));
    assert(latest.summary(2000000).find("Кружок") == 0);
    assert(latest.lines(2000000)[0].find("Кружок") == 0);

    TdEventQueue queue;
    for (int i = 1; i <= 1000; ++i)
        enqueue_td_event(queue, json_pack("{s:s,s:{s:i}}", "@type", "updateFile", "file", "id", i));
    enqueue_td_event(queue, parse(R"({"@type":"message","@extra":{"kind":"media_upload","token":1}})"));
    enqueue_td_event(queue, parse(R"({"@type":"updateMessageSendSucceeded"})"));
    auto* first = static_cast<json_t*>(take_td_event(queue, true, false));
    assert(MediaUploads::string(first, "@type") == "message"); json_decref(first);
    auto* terminal = static_cast<json_t*>(take_td_event(queue, true, false));
    assert(MediaUploads::string(terminal, "@type") == "updateMessageSendSucceeded"); json_decref(terminal);
    assert(queue.size() == 1000 && !take_td_event(queue, true, false));
    // Background events get service even when urgent work keeps arriving.
    enqueue_td_event(queue, parse(R"({"@type":"updateMessageSendSucceeded"})"));
    auto* background = static_cast<json_t*>(take_td_event(queue, false, true));
    assert(MediaUploads::string(background, "@type") == "updateFile"); json_decref(background);
    for (auto* value : queue) json_decref(static_cast<json_t*>(value));
    assert(td_reply_is_foreground("{\"@type\":\"messages\",\"total_count\":1,\"@extra\":{\"first\":true,\"kind\":\"history\",\"chat\":5}}"));
    assert(td_reply_is_foreground("{\"@type\":\"ok\",\"@extra\":{\"kind\":\"reaction_add\"}}"));
    assert(td_reply_is_foreground("{\"@type\":\"ok\",\"@extra\":{\"chat\":1,\"kind\":\"forum_check\"}}"));
    assert(!td_reply_is_foreground("{\"@type\":\"updateChatLastMessage\",\"chat_id\":1}"));
    assert(!td_reply_is_foreground("{\"@type\":\"ok\",\"@extra\":{\"kind\":\"sticker_packs\"}}"));
    assert(!td_reply_is_foreground(nullptr));
    std::puts("Upload status: completion, reordered events, receipt-time speed, stickers and priority/fairness passed");
}
