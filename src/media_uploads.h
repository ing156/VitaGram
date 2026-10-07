#pragma once
#include <jansson.h>
#include "i18n.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <utility>

// UI-owned state. No filesystem or TDLib calls; timestamps are monotonic microseconds.
class MediaUploads {
public:
    enum class Kind { Audio, VideoNote, Sticker };
    struct Item {
        int64_t token = 0, chat = 0, message = 0, size = 0, done = 0;
        int64_t started = 0, changed = 0, ended = 0, sample_time = 0, sample_bytes = 0;
        int file = 0;
        bool video = false, active = false, uploaded = false, terminal = false;
        bool dismissed = false;
        bool reused = false;
        Kind kind = Kind::Audio;
        int64_t file_time = 0;
        double speed = 0;
        std::string path, error;
    };
    json_t* begin(const std::string& path, int64_t size, bool video, int64_t chat, int64_t now) {
        items_.erase(std::remove_if(items_.begin(), items_.end(), [now](const Item& i) {
            return i.terminal && now - i.ended > 15000000;
        }), items_.end());
        Item i; i.token = ++serial_; i.path = path; i.size = size; i.video = video; i.chat = chat;
        i.kind = video ? Kind::VideoNote : Kind::Audio;
        i.started = i.changed = now;
        items_.push_back(i);
        return json_pack("{s:s,s:I}", "kind", "media_upload", "token", (json_int_t)i.token);
    }
    json_t* beginSticker(int file, int64_t chat, int64_t now) {
        auto* extra = begin("", 0, false, chat, now);
        auto& i = items_.back(); i.kind = Kind::Sticker;
        i.file = file; i.reused = i.uploaded = true;
        return extra;
    }
    static int64_t number(json_t* o, const char* k) {
        json_t* v = json_object_get(o, k);
        return json_is_string(v) ? std::strtoll(json_string_value(v), nullptr, 10) : json_integer_value(v);
    }
    static std::string string(json_t* o, const char* k) {
        const char* s = json_string_value(json_object_get(o, k)); return s ? s : "";
    }
    static json_t* messageFile(json_t* message) {
        json_t* content = json_object_get(message, "content");
        for (const auto& field : {std::make_pair("video_note", "video"), std::make_pair("voice_note", "voice"),
                                 std::make_pair("sticker", "sticker"), std::make_pair("audio", "audio"),
                                 std::make_pair("document", "document"), std::make_pair("video", "video")})
            if (auto* value = json_object_get(content, field.first)) return json_object_get(value, field.second);
        return nullptr;
    }
    void file(json_t* file, int64_t now) {
        int id = (int)number(file, "id");
        if (!id) return;
        auto path = string(json_object_get(file, "local"), "path");
        for (auto& i : items_) if (!i.terminal && (i.file == id || (!path.empty() && i.path == path))) {
            if (now < i.file_time) continue;
            i.file_time = now;
            i.file = id;
            int64_t size = std::max(number(file, "size"), number(file, "expected_size"));
            if (size > 0) i.size = size;
            json_t* remote = json_object_get(file, "remote");
            int64_t done = std::max<int64_t>(0, number(remote, "uploaded_size"));
            if (done != i.done) i.changed = now;
            if (!i.sample_time || done < i.done) { i.sample_time = now; i.sample_bytes = done; i.speed = 0; }
            i.done = done;
            if (now - i.sample_time >= 1000000) {
                double speed = (done - i.sample_bytes) * 1000000.0 / (now - i.sample_time);
                i.speed = i.speed ? i.speed * .5 + speed * .5 : speed;
                i.sample_time = now; i.sample_bytes = done;
            }
            i.active = json_is_true(json_object_get(remote, "is_uploading_active"));
            i.uploaded = i.uploaded || json_is_true(json_object_get(remote, "is_uploading_completed"));
            if (i.uploaded) i.done = std::max(i.done, i.size);
            if (!i.active || i.uploaded) i.speed = 0;
        }
    }
    void process(json_t* root, int64_t now) {
        const auto type = string(root, "@type");
        json_t* extra = json_object_get(root, "@extra");
        if (string(extra, "kind") == "media_upload") {
            for (auto& i : items_) if (i.token == number(extra, "token") && !i.terminal) {
                if (type == "error") { finish(i, string(root, "message"), now); }
                else if (type == "message") {
                    i.chat = number(root, "chat_id"); i.message = number(root, "id");
                    i.file = (int)number(messageFile(root), "id");
                    file(messageFile(root), now);
                    // Already-sent/reused files can bypass the pending-message phase.
                    json_t* state = json_object_get(root, "sending_state");
                    if (!json_is_object(state)) finish(i, "", now);
                    else if (string(state, "@type") == "messageSendingStateFailed") {
                        auto error = string(json_object_get(state, "error"), "message");
                        finish(i, error.empty() ? tr("up.unknown_error") : error, now);
                    }
                    for (const auto& ended : ended_) if (!i.terminal && ended.chat == i.chat && ended.message == i.message)
                        finish(i, ended.error, ended.time);
                }
            }
            if (type == "message") file(messageFile(root), now);
        }
        if (type == "updateNewMessage") {
            auto* message = json_object_get(root, "message");
            auto* state = json_object_get(message, "sending_state");
            const int64_t sending_id = number(state, "sending_id");
            for (auto& i : items_) if (!i.terminal && sending_id && sending_id == i.token && i.chat == number(message, "chat_id")) {
                i.message = number(message, "id"); i.file = (int)number(messageFile(message), "id");
                file(messageFile(message), now);
            }
        }
        if (type == "updateFile") file(json_object_get(root, "file"), now);
        else if (type == "file") file(root, now);
        else if (type == "updateMessageSendSucceeded" || type == "updateMessageSendFailed") {
            json_t* m = json_object_get(root, "message");
            const int64_t chat = number(m, "chat_id"), old = number(root, "old_message_id");
            auto error = type == "updateMessageSendFailed" ? string(json_object_get(root, "error"), "message") : "";
            if (type == "updateMessageSendFailed" && error.empty()) error = tr("up.unknown_error");
            file(messageFile(m), now);
            if (chat && old) {
                if (ended_.size() >= 64) ended_.erase(ended_.begin());
                ended_.push_back({chat, old, now, error});
            }
            for (auto& i : items_) if (!i.terminal && i.message && i.chat == number(m, "chat_id") &&
                i.message == number(root, "old_message_id")) {
                finish(i, error, now);
            }
        }
    }
    std::vector<std::string> lines(int64_t now, bool diagnostic = false) const {
        std::vector<std::string> result;
        // Current work first; recently completed operations yield immediately.
        for (bool terminal : {false, true}) for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
            const auto& i = *it;
            if (i.terminal == terminal && (!i.terminal ||
                (now - i.ended < (diagnostic ? 15000000 : 3000000) && (diagnostic || !i.dismissed)))) {
                result.push_back(describe(i, now));
                if (i.reused) continue;
                result.push_back(tr("up.data_line", {std::to_string(i.done), std::to_string(i.size),
                    std::to_string(std::max<int64_t>(0, i.size-i.done))}));
            }
        }
        return result;
    }
    void dismissCompleted() {
        for (auto& i : items_) if (i.terminal) i.dismissed = true;
    }
    std::string summary(int64_t now, int64_t chat = 0) const {
        size_t active = 0; const Item* selected = nullptr;
        for (auto i = items_.rbegin(); i != items_.rend(); ++i) if ((!chat || i->chat == chat) && !i->terminal) {
            ++active; if (!selected) selected = &*i;
        }
        if (!selected) for (auto i = items_.rbegin(); i != items_.rend(); ++i)
            if ((!chat || i->chat == chat) && i->terminal && !i->dismissed && now - i->ended < 3000000) {
                selected = &*i; break;
            }
        return selected ? describe(*selected, now) + (active > 1 ? tr("up.more", {std::to_string(active - 1)}) : "") : "";
    }
    const std::vector<Item>& items() const { return items_; }
private:
    int64_t serial_ = 0;
    std::vector<Item> items_;
    struct Ended { int64_t chat, message, time; std::string error; };
    std::vector<Ended> ended_;
    static void finish(Item& i, const std::string& error, int64_t now) {
        i.terminal = true; i.ended = now; i.error = error; i.active = false;
        if (error.empty()) { i.done = i.size; i.uploaded = true; }
    }
    static std::string bytes(int64_t n) {
        char b[48];
        if (n >= 1048576) { std::snprintf(b, sizeof(b), "%.2f", n / 1048576.0); return tr("unit.mib", {b}); }
        std::snprintf(b, sizeof(b), "%.1f", n / 1024.0);
        return tr("unit.kib", {b});
    }
    static std::string seconds(int64_t micros) { return tr("unit.sec", {std::to_string(micros / 1000000)}); }
    static std::string describe(const Item& i, int64_t now) {
        std::string s = tr(i.kind == Kind::Sticker ? "up.kind.sticker" : i.video ? "up.kind.video_note" : "up.kind.audio");
        if (i.terminal && i.reused && i.error.empty()) return tr("up.sent", {s});
        if (i.terminal) return i.error.empty() ? tr("up.sent_info", {s, bytes(i.size), seconds(i.ended-i.started)})
                                               : tr("up.failed", {s, i.error});
        if (i.reused) return tr("up.confirming", {s, seconds(now-i.started)});
        if (!i.file) return tr("up.awaiting", {s, seconds(now-i.started)});
        s = tr("up.progress", {s, bytes(i.done), bytes(i.size),
            std::to_string(i.size ? std::min<int64_t>(100, i.done * 100 / i.size) : 0)});
        int64_t idle = std::max<int64_t>(0, now-i.changed)/1000000;
        if (i.uploaded) s += tr("up.s.confirm");
        else if (i.size > 0 && i.done >= i.size) s += tr("up.s.finishing");
        else if (idle >= 5) s += tr("up.s.stalled", {tr("unit.sec", {std::to_string(idle)})});
        else if (i.active && i.speed > 0) s += tr("up.s.speed", {tr("unit.per_sec", {bytes((int64_t)i.speed)}),
            tr("unit.sec", {std::to_string((int64_t)(std::max<int64_t>(0, i.size-i.done)/i.speed) + 1)})});
        else s += tr(i.active ? "up.s.uploading" : "up.s.queued");
        return s + tr("up.elapsed", {seconds(now-i.started)});
    }
};
