#pragma once
#include <jansson.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// UI-owned state. No filesystem or TDLib calls; timestamps are monotonic microseconds.
class MediaUploads {
public:
    struct Item {
        int64_t token = 0, chat = 0, message = 0, size = 0, done = 0;
        int64_t started = 0, changed = 0, ended = 0, sample_time = 0, sample_bytes = 0;
        int file = 0;
        bool video = false, active = false, uploaded = false, terminal = false;
        double speed = 0;
        std::string path, error;
    };
    json_t* begin(const std::string& path, int64_t size, bool video, int64_t chat, int64_t now) {
        items_.erase(std::remove_if(items_.begin(), items_.end(), [now](const Item& i) {
            return i.terminal && now - i.ended > 15000000;
        }), items_.end());
        Item i; i.token = ++serial_; i.path = path; i.size = size; i.video = video; i.chat = chat;
        i.started = i.changed = i.sample_time = now;
        items_.push_back(i);
        return json_pack("{s:s,s:I}", "kind", "media_upload", "token", (json_int_t)i.token);
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
        json_t* note = json_object_get(content, "video_note");
        return note ? json_object_get(note, "video") : json_object_get(json_object_get(content, "voice_note"), "voice");
    }
    void file(json_t* file, int64_t now) {
        int id = (int)number(file, "id");
        if (!id) return;
        auto path = string(json_object_get(file, "local"), "path");
        for (auto& i : items_) if (!i.terminal && (i.file == id || (!path.empty() && i.path == path))) {
            i.file = id;
            int64_t size = std::max(number(file, "size"), number(file, "expected_size"));
            if (size > 0) i.size = size;
            json_t* remote = json_object_get(file, "remote");
            int64_t done = std::max<int64_t>(0, number(remote, "uploaded_size"));
            if (done != i.done) i.changed = now;
            if (done < i.done) { i.sample_time = now; i.sample_bytes = done; i.speed = 0; }
            i.done = done;
            if (now - i.sample_time >= 1000000) {
                double speed = (done - i.sample_bytes) * 1000000.0 / (now - i.sample_time);
                i.speed = i.speed ? i.speed * .5 + speed * .5 : speed;
                i.sample_time = now; i.sample_bytes = done;
            }
            i.active = json_is_true(json_object_get(remote, "is_uploading_active"));
            i.uploaded = json_is_true(json_object_get(remote, "is_uploading_completed"));
            if (i.uploaded) i.done = std::max(i.done, i.size);
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
                    // Already-sent/reused files can bypass the pending-message phase.
                    json_t* state = json_object_get(root, "sending_state");
                    if (json_is_null(state)) finish(i, "", now);
                }
            }
            if (type == "message") file(messageFile(root), now);
        }
        if (type == "updateFile") file(json_object_get(root, "file"), now);
        else if (type == "file") file(root, now);
        else if (type == "updateMessageSendSucceeded" || type == "updateMessageSendFailed") {
            json_t* m = json_object_get(root, "message");
            for (auto& i : items_) if (!i.terminal && i.message && i.chat == number(m, "chat_id") &&
                i.message == number(root, "old_message_id")) {
                finish(i, type == "updateMessageSendSucceeded" ? "" : string(json_object_get(root, "error"), "message"), now);
                if (type == "updateMessageSendFailed" && i.error.empty()) i.error = "неизвестная ошибка";
            }
        }
    }
    std::vector<std::string> lines(int64_t now) const {
        std::vector<std::string> result;
        for (const auto& i : items_) if (!i.terminal || now - i.ended < 15000000) {
            result.push_back(describe(i, now));
            result.push_back("Данных: " + std::to_string(i.done) + "/" + std::to_string(i.size) +
                " Б; осталось: " + std::to_string(std::max<int64_t>(0, i.size-i.done)) + " Б");
        }
        return result;
    }
    std::string summary(int64_t now) const {
        size_t active = 0; const Item* selected = nullptr;
        for (const auto& i : items_) if (!i.terminal) { ++active; if (!selected) selected = &i; }
        if (!selected && !items_.empty() && now - items_.back().ended < 15000000) selected = &items_.back();
        return selected ? describe(*selected, now) + (active > 1 ? " | ещё: " + std::to_string(active - 1) : "") : "";
    }
    const std::vector<Item>& items() const { return items_; }
private:
    int64_t serial_ = 0;
    std::vector<Item> items_;
    static void finish(Item& i, const std::string& error, int64_t now) {
        i.terminal = true; i.ended = now; i.error = error; i.active = false;
        if (error.empty()) { i.done = i.size; i.uploaded = true; }
    }
    static std::string bytes(int64_t n) {
        char b[48];
        if (n >= 1048576) std::snprintf(b, sizeof(b), "%.2f МиБ", n / 1048576.0);
        else std::snprintf(b, sizeof(b), "%.1f КиБ", n / 1024.0);
        return b;
    }
    static std::string describe(const Item& i, int64_t now) {
        std::string s = i.video ? "Кружок" : "Аудио";
        if (i.terminal) return s + (i.error.empty() ? ": отправлено (" + bytes(i.size) + ", " +
            std::to_string((i.ended-i.started)/1000000) + " с)" : ": ошибка — " + i.error);
        s += ": " + bytes(i.done) + "/" + bytes(i.size) + " (" +
            std::to_string(i.size ? std::min<int64_t>(100, i.done * 100 / i.size) : 0) + "%)";
        int64_t idle = std::max<int64_t>(0, now-i.changed)/1000000;
        if (i.uploaded) s += " — подтверждение Telegram";
        else if (idle >= 5) s += " — нет прогресса " + std::to_string(idle) + " с";
        else if (i.active && i.speed > 0) s += " | " + bytes((int64_t)i.speed) + "/с | ~" +
            std::to_string((int64_t)(std::max<int64_t>(0, i.size-i.done)/i.speed) + 1) + " с";
        else s += i.active ? " — загрузка" : " — ожидание загрузки";
        return s + " | прошло " + std::to_string((now-i.started)/1000000) + " с";
    }
};
