#pragma once
#include <jansson.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>
#include <cerrno>
#include "inline_thumbnail.h"

struct SendGifItem {
    int32_t file = 0, preview = 0;
    int width = 0, height = 0, duration = 0;
    int64_t query_id = 0;
    bool video_preview = false;
    std::string result_id, title, path, fallback;
    std::string preview_format;
    int preview_width = 0, preview_height = 0;
    int64_t preview_size = 0;
    bool preview_reported = false;
};

// Session-only IDs: neither inline query IDs nor file IDs can be restored from disk.
struct GifCatalog {
    static constexpr size_t kLimit = 240;
    std::vector<SendGifItem> items;
    std::string query, next_offset;
    unsigned generation = 0;
    int64_t chat = 0, thread = 0;
    int32_t topic = 0;
    bool pending = false, end = false;
    int64_t deadline = 0;
    void begin(int64_t now) { pending = true; deadline = now + 30000000; }
    void finish() { pending = false; deadline = 0; }
    bool expire(int64_t now) {
        if (!pending || !deadline || now < deadline) return false;
        finish(); ++generation; return true; // Late replies cannot complete a retry.
    }

    static const char* string(json_t* obj, const char* key) {
        const char* value = json_string_value(json_object_get(obj, key));
        return value ? value : "";
    }
    static int64_t integer(json_t* obj, const char* key) {
        auto* value = json_object_get(obj, key);
        if (json_is_integer(value)) return json_integer_value(value);
        const char* text = json_string_value(value);
        if (!text || !*text || (*text != '-' && (*text < '0' || *text > '9'))) return 0;
        char* end = nullptr; errno = 0;
        const auto parsed = std::strtoll(text, &end, 10);
        return errno == ERANGE || end == text || *end ? 0 : parsed;
    }
    void reset(int64_t target, int32_t forum, int64_t discussion, const std::string& text) {
        ++generation; chat = target; topic = forum; thread = discussion;
        query = text; items.clear(); next_offset.clear(); finish(); end = false;
    }
    json_t* extra(const char* kind, const std::string& offset = "") const {
        return json_pack("{s:s,s:I,s:i,s:I,s:I,s:s}", "kind", kind, "chat", (json_int_t)chat,
                         "topic", topic, "thread", (json_int_t)thread, "generation", (json_int_t)generation,
                         "offset", offset.c_str());
    }
    bool matches(json_t* value, int64_t target, int32_t forum, int64_t discussion) const {
        return chat == target && topic == forum && thread == discussion &&
            integer(value, "chat") == chat && integer(value, "topic") == topic &&
            integer(value, "thread") == thread && integer(value, "generation") == generation;
    }
    static bool videoPreview(json_t* animation) {
        const char* format = string(json_object_get(json_object_get(animation, "thumbnail"), "format"), "@type");
        return !std::strcmp(format, "thumbnailFormatMpeg4") || !std::strcmp(format, "thumbnailFormatGif");
    }
    static std::string previewPath(bool video, const char* path) {
        return !path || !*path ? "" : video ? std::string("video-thumb:") + path : path;
    }
    static std::string previewPath(const SendGifItem& item, const char* path) {
        return previewPath(item.video_preview, path);
    }
    // Download the separate thumbnail, including MP4/GIF; never the full animation.
    static json_t* previewFile(json_t* animation) {
        json_t* thumbnail = json_object_get(animation, "thumbnail");
        const char* format = string(json_object_get(thumbnail, "format"), "@type");
        return !std::strcmp(format, "thumbnailFormatJpeg") || !std::strcmp(format, "thumbnailFormatPng") ||
               !std::strcmp(format, "thumbnailFormatWebp") || videoPreview(animation) ? json_object_get(thumbnail, "file") : nullptr;
    }
    void append(json_t* root, bool saved, const std::string& offset) {
        size_t index; json_t* result;
        json_array_foreach(json_object_get(root, saved ? "animations" : "results"), index, result) {
            if (items.size() >= kLimit) break;
            if (!saved && std::strcmp(string(result, "@type"), "inlineQueryResultAnimation")) continue;
            json_t* animation = saved ? result : json_object_get(result, "animation");
            SendGifItem item;
            item.file = (int32_t)integer(json_object_get(animation, "animation"), "id");
            item.width = (int)integer(animation, "width"); item.height = (int)integer(animation, "height");
            item.duration = (int)integer(animation, "duration");
            item.query_id = saved ? 0 : integer(root, "inline_query_id");
            item.result_id = saved ? "" : string(result, "id");
            item.title = saved ? string(animation, "file_name") : string(result, "title");
            if ((saved && item.file <= 0) || (!saved && (!item.query_id || item.result_id.empty()))) continue;
            if (std::any_of(items.begin(), items.end(), [&](const SendGifItem& other) {
                    return (item.file > 0 && other.file == item.file) ||
                        (!saved && other.query_id == item.query_id && other.result_id == item.result_id);
                })) continue;
            json_t* preview = previewFile(animation);
            auto* thumbnail = json_object_get(animation, "thumbnail");
            item.preview_format = string(json_object_get(thumbnail, "format"), "@type");
            item.preview_width = (int)integer(thumbnail, "width");
            item.preview_height = (int)integer(thumbnail, "height");
            item.preview_size = std::max(integer(preview, "size"), integer(preview, "expected_size"));
            item.video_preview = videoPreview(animation);
            item.preview = (int32_t)integer(preview, "id");
            json_t* local = json_object_get(preview, "local");
            if (json_is_true(json_object_get(local, "is_downloading_completed"))) item.path = previewPath(item, string(local, "path"));
            item.fallback = inline_thumbnail::key(string(json_object_get(animation, "minithumbnail"), "data"));
            items.push_back(std::move(item));
        }
        next_offset = saved ? "" : string(root, "next_offset");
        end = saved || next_offset.empty() || next_offset == offset || items.size() >= kLimit;
        finish();
    }
    json_t* sendRequest(size_t index) const {
        if (index >= items.size()) return nullptr;
        const auto& item = items[index];
        json_t* request;
        if (item.query_id) {
            request = json_pack("{s:s,s:I,s:s,s:s,s:b}", "@type", "sendInlineQueryResultMessage",
                "chat_id", (json_int_t)chat, "query_id", std::to_string(item.query_id).c_str(),
                "result_id", item.result_id.c_str(), "hide_via_bot", 1);
        } else {
            json_t* animation = json_pack("{s:s,s:{s:s,s:i},s:i,s:i,s:i}", "@type", "inputAnimation",
                "animation", "@type", "inputFileId", "id", item.file,
                "width", item.width, "height", item.height, "duration", item.duration);
            json_t* content = json_pack("{s:s,s:o}", "@type", "inputMessageAnimation", "animation", animation);
            request = json_pack("{s:s,s:I,s:o}", "@type", "sendMessage", "chat_id", (json_int_t)chat,
                "input_message_content", content);
        }
        if (thread) json_object_set_new(request, "topic_id", json_pack("{s:s,s:I}", "@type", "messageTopicThread", "message_thread_id", (json_int_t)thread));
        else if (topic) json_object_set_new(request, "topic_id", json_pack("{s:s,s:i}", "@type", "messageTopicForum", "forum_topic_id", topic));
        json_object_set_new(request, "@extra", json_string("send_message"));
        return request;
    }
};
