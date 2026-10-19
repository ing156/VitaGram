#include "gif_catalog.h"
#include "td_event_queue.h"
#include "gif_diagnostics.h"
#include <cassert>
#include <cstdio>

static json_t* parse(const char* text) {
    json_error_t error{};
    auto* value = json_loads(text, 0, &error);
    assert(value); return value;
}

int main() {
    GifCatalog catalog;
    catalog.reset(100, 7, 0, "cats");
    auto* extra = catalog.extra("gif_results");
    assert(catalog.matches(extra, 100, 7, 0));
    assert(!catalog.matches(extra, 101, 7, 0));
    assert(!catalog.matches(extra, 100, 8, 0));
    assert(!catalog.matches(extra, 100, 7, 44));
    catalog.reset(100, 7, 0, "dogs");
    assert(!catalog.matches(extra, 100, 7, 0)); // old query in the same chat
    json_decref(extra);

    auto* page = parse(R"({"inline_query_id":"9007199254740993","next_offset":"page2","results":[
        {"@type":"inlineQueryResultAnimation","id":"one","title":"Dog","animation":{
          "animation":{"id":21,"local":{"path":"dog.mp4","is_downloading_completed":true}},"width":320,"height":180,"duration":2,
          "thumbnail":{"format":{"@type":"thumbnailFormatJpeg"},"file":{"id":22,
            "local":{"is_downloading_completed":true,"path":"dog.jpg"}}}}},
        {"@type":"inlineQueryResultPhoto","id":"ignore"},
        {"@type":"inlineQueryResultAnimation","id":"one","animation":{"animation":{"id":21}}},
        {"@type":"inlineQueryResultAnimation","id":"two","animation":{
          "minithumbnail":{"data":"/9j/"},"thumbnail":{"format":{"@type":"thumbnailFormatMpeg4"},"file":{"id":23}}}},
        {"@type":"inlineQueryResultAnimation","id":"","animation":{"animation":{"id":24}}}
    ]})");
    catalog.pending = true;
    catalog.append(page, false, "");
    assert(!catalog.pending && !catalog.end && catalog.next_offset == "page2");
    assert(catalog.items.size() == 2);
    assert(catalog.items[0].preview == 22 && catalog.items[0].path == "dog.jpg");
    assert(catalog.items[0].full_path == "dog.mp4");
    assert(catalog.items[1].fallback == "inline-jpeg:/9j/");
    assert(catalog.items[1].preview == 23 && catalog.items[1].video_preview);
    assert(GifCatalog::previewPath(catalog.items[1], "preview.mp4") == "video-thumb:preview.mp4");
    assert(GifCatalog::previewPath(catalog.items[1], "").empty());
    auto* gif_preview = parse(R"({"thumbnail":{"format":{"@type":"thumbnailFormatGif"},"file":{"id":25}}})");
    assert(GifCatalog::videoPreview(gif_preview));
    assert(GifCatalog::integer(GifCatalog::previewFile(gif_preview), "id") == 25);
    json_decref(gif_preview);
    auto* send = catalog.sendRequest(0);
    assert(!std::strcmp(GifCatalog::string(send, "@type"), "sendInlineQueryResultMessage"));
    assert(GifCatalog::integer(send, "query_id") == 9007199254740993LL);
    assert(!std::strcmp(GifCatalog::string(send, "result_id"), "one"));
    assert(GifCatalog::integer(json_object_get(send, "topic_id"), "forum_topic_id") == 7);
    assert(json_is_true(json_object_get(send, "hide_via_bot")));
    json_decref(send);

    auto* second = parse(R"({"inline_query_id":99,"next_offset":"page2","results":[
      {"@type":"inlineQueryResultAnimation","id":"three","animation":{"animation":{"id":31,"local":{"path":"partial.mp4","is_downloading_completed":false}}}}]})");
    catalog.append(second, false, "page2");
    assert(catalog.end && catalog.items.size() == 3); // repeating cursor cannot loop
    assert(catalog.items[0].query_id == 9007199254740993LL && catalog.items[2].query_id == 99);
    assert(catalog.items[2].full_path.empty());
    json_decref(second); json_decref(page);

    catalog.reset(100, 0, 1234, "");
    auto* saved = parse(R"({"animations":[{"animation":{"id":51,"local":{"path":"saved.mp4","is_downloading_completed":true}},"width":640,"height":360,"duration":3,
      "thumbnail":{"format":{"@type":"thumbnailFormatWebp"},"file":{"id":52,"local":{"path":"partial.webp","is_downloading_completed":false}}}},
      {"animation":{"id":0}}]})");
    catalog.append(saved, true, "");
    assert(catalog.end && catalog.items.size() == 1 && catalog.items[0].path.empty());
    assert(catalog.items[0].full_path == "saved.mp4");
    send = catalog.sendRequest(0);
    assert(!std::strcmp(GifCatalog::string(send, "@type"), "sendMessage"));
    auto* content = json_object_get(send, "input_message_content");
    assert(!std::strcmp(GifCatalog::string(content, "@type"), "inputMessageAnimation"));
    auto* input = json_object_get(content, "animation");
    assert(GifCatalog::integer(json_object_get(input, "animation"), "id") == 51);
    assert(GifCatalog::integer(input, "width") == 640);
    assert(GifCatalog::integer(json_object_get(send, "topic_id"), "message_thread_id") == 1234);
    json_decref(send); json_decref(saved);
    assert(!catalog.sendRequest(1));

    catalog.reset(100, 0, 0, "many");
    page = json_pack("{s:I,s:s,s:[]}", "inline_query_id", (json_int_t)10, "next_offset", "more", "results");
    auto* results = json_object_get(page, "results");
    for (int i = 1; i <= 300; ++i) {
        const std::string id = std::to_string(i);
        json_array_append_new(results, json_pack("{s:s,s:s,s:{s:{s:i}}}", "@type", "inlineQueryResultAnimation", "id", id.c_str(), "animation", "animation", "id", i));
    }
    catalog.append(page, false, "");
    assert(catalog.items.size() == GifCatalog::kLimit && catalog.end);
    json_decref(page);
    assert(GifCatalog::integer(nullptr, "id") == 0);
    auto* ints = parse(R"({"max":"9223372036854775807","min":"-9223372036854775808","bad":"12x","overflow":"9223372036854775808"})");
    assert(GifCatalog::integer(ints, "max") == INT64_MAX);
    assert(GifCatalog::integer(ints, "min") == INT64_MIN);
    assert(!GifCatalog::integer(ints, "bad") && !GifCatalog::integer(ints, "overflow"));
    json_decref(ints);
    auto* late = catalog.extra("gif_saved");
    catalog.begin(100);
    assert(!catalog.expire(30000099));
    assert(catalog.expire(30000100) && !catalog.pending && !catalog.deadline);
    assert(!catalog.matches(late, 100, 0, 0));
    assert(!catalog.expire(60000000));
    json_decref(late);
    catalog.begin(200); catalog.finish(); assert(!catalog.expire(90000000));
    auto* reply = parse(R"({"@type":"animations"})");
    json_object_set_new(reply, "@extra", catalog.extra("gif_saved"));
    const auto metadata = describe_td_event(reply);
    assert(metadata.urgent && metadata.chat == 100 && metadata.response == TdResponseKind::Gif);
    TdEventQueue queue;
    queue.push_back(reply);
    assert(queue.take(true, false, 100) == reply); // delivered even during touch interaction
    char* wire = json_dumps(reply, JSON_COMPACT);
    assert(td_reply_is_foreground(wire));
    // Logging is off by default. Enable only GIF diagnostics for the ring checks.
    gif_diagnostics::raw("received", wire, 555);
    assert(gif_diagnostics::ring().count == 0);
    diag_log::set_mask(1u << static_cast<unsigned>(diag_log::Kind::Gif));
    gif_diagnostics::raw("received", wire, 555);
    auto& ring = gif_diagnostics::ring();
    assert(ring.count == 1 && ring.events[0].generation == catalog.generation);
    assert(!std::strcmp(ring.events[0].kind, "gif_saved"));
    gif_diagnostics::preview("preview_failed", "thumbnailFormatMpeg4", 9, 3, 23, 320, 180, 4096, "video_no_frame", 556, 1);
    assert(ring.count == 2 && ring.events[1].item == 3 && ring.events[1].file == 23);
    assert(ring.events[1].width == 320 && ring.events[1].bytes == 4096);
    assert(!std::strcmp(ring.events[1].reason, "video_no_frame"));
    std::free(wire); json_decref(reply);
    std::vector<uint8_t> bytes;
    assert(inline_thumbnail::decode("inline-jpeg:/9j/", bytes) && bytes.size() == 3);
    assert(!inline_thumbnail::decode("inline-jpeg:/9j/!!!!", bytes) && bytes.empty());
    assert(!inline_thumbnail::decode("inline-jpeg:/9j/AA=A", bytes));
    assert(inline_thumbnail::key("not-base64").empty());
    catalog.reset(0, 0, 0, "");
    assert(catalog.items.empty() && !catalog.sendRequest(0));
    std::puts("GIF catalog: scopes, thumbnails, pagination, limits and send requests: PASS");
}
