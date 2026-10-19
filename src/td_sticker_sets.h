#pragma once
#include <cstring>
#include "td_unused_update.h"
#include "sticker_preview.h"

// getInstalledStickerSets replies with ~300 KB of full cover stickers, but the
// pack list needs only id, title and one static preview per set. Skip the rest
// at grammar level so the Parser thread never builds (or frees) that DOM.
// Any surprise returns nullptr and the ordinary Jansson path runs instead.
namespace td_sets_detail {
// Jansson's loader costs tens of microseconds per call on the Vita and a sticker
// reply has thousands of members, so scalars the Reader already validated are
// built directly; anything else (escapes, reals, containers) goes through it.
inline json_t* load(const char* b, const char* e) {
    const size_t n = e - b;
    if (n >= 2 && *b == '"' && !std::memchr(b, '\\', n)) return json_stringn(b + 1, n - 2);
    if (n == 4 && !std::memcmp(b, "true", 4)) return json_true();
    if (n == 5 && !std::memcmp(b, "false", 5)) return json_false();
    if (n == 4 && !std::memcmp(b, "null", 4)) return json_null();
    if (n >= 1 && n <= 18 && (*b == '-' || (*b >= '0' && *b <= '9'))) {
        const char* p = b + (*b == '-');
        long long v = 0;
        bool whole = p < e;
        for (; p < e; ++p) { if (*p < '0' || *p > '9') { whole = false; break; } v = v * 10 + (*p - '0'); }
        if (whole) return json_integer(*b == '-' ? -v : v);
    }
    return json_loadb(b, n, JSON_DECODE_ANY, nullptr);
}
inline bool key(td_unused_detail::Reader& r, std::string& out) {
    r.ws(); const char* b = r.p;
    if (!r.string() || r.p - b > 256) return false;
    if (!std::memchr(b, '\\', r.p - b)) out.assign(b + 1, r.p - 1);
    else {
        auto* k = json_loadb(b, r.p - b, JSON_DECODE_ANY, nullptr);
        const char* s = json_string_value(k);
        if (s) out = s;
        json_decref(k);
        if (!s) return false;
    }
    r.ws();
    if (r.p == r.end || *r.p++ != ':') return false;
    r.ws();
    return true;
}
inline json_t* set_entry(td_unused_detail::Reader& r) {
    if (r.p == r.end || *r.p++ != '{') return nullptr;
    json_t* entry = json_object(); json_t* covers = nullptr; bool ok = false, found = false;
    json_object_set_new(entry, "@type", json_string("stickerSetInfo"));
    r.ws();
    if (r.p < r.end && *r.p == '}') { ++r.p; ok = true; }
    while (!ok) {
        std::string name;
        if (!key(r, name)) break;
        const char* b = r.p;
        if (name == "covers") {
            if (r.p == r.end || *r.p++ != '[') break;
            covers = json_array(); r.ws();
            bool closed = false;
            if (r.p < r.end && *r.p == ']') { ++r.p; closed = true; }
            bool bad = false;
            while (!closed) {
                r.ws(); const char* eb = r.p;
                if (!r.value()) { bad = true; break; }
                if (!found) {
                    if (auto* el = load(eb, r.p)) {
                        auto* probe = json_pack("{s:[O]}", "covers", el);
                        if (sticker_pack_preview(probe)) { json_array_append(covers, el); found = true; }
                        json_decref(probe); json_decref(el);
                    }
                }
                r.ws();
                if (r.p == r.end) { bad = true; break; }
                if (*r.p == ']') { ++r.p; closed = true; }
                else if (*r.p++ != ',') { bad = true; break; }
            }
            if (bad) break;
        } else if (!r.value()) break;
        else if (name == "id" || name == "title" || name == "thumbnail") {
            if (auto* v = load(b, r.p)) json_object_set_new(entry, name.c_str(), v);
        }
        r.ws();
        if (r.p == r.end) break;
        if (*r.p == '}') { ++r.p; ok = true; }
        else if (*r.p++ != ',') break;
    }
    if (!ok) { json_decref(entry); json_decref(covers); return nullptr; }
    if (covers) json_object_set_new(entry, "covers", covers);
    return entry;
}
}
inline json_t* compact_raw_sticker_sets(const std::string& text) {
    using namespace td_sets_detail;
    if (text.compare(0, 1, "{") || text.size() < 4096 ||
        std::string_view(text.data(), 128).find("stickerSets") == std::string_view::npos ||
        text.find("sticker_packs") == std::string::npos) return nullptr;
    td_unused_detail::Reader r{text.data(), text.data() + text.size()};
    ++r.p;
    json_t* root = json_object(); bool ok = false;
    for (;;) {
        std::string name;
        if (!key(r, name)) break;
        if (name == "sets") {
            if (r.p == r.end || *r.p++ != '[') break;
            auto* sets = json_array(); json_object_set_new(root, "sets", sets);
            r.ws(); bool closed = false, bad = false;
            if (r.p < r.end && *r.p == ']') { ++r.p; closed = true; }
            while (!closed) {
                r.ws();
                auto* entry = set_entry(r);
                if (!entry) { bad = true; break; }
                json_array_append_new(sets, entry);
                r.ws();
                if (r.p == r.end) { bad = true; break; }
                if (*r.p == ']') { ++r.p; closed = true; }
                else if (*r.p++ != ',') { bad = true; break; }
            }
            if (bad) break;
        } else {
            const char* b = r.p;
            if (!r.value()) break;
            if (name == "@type" || name == "@extra") {
                auto* v = load(b, r.p);
                if (!v) break;
                json_object_set_new(root, name.c_str(), v);
            }
        }
        r.ws();
        if (r.p == r.end) break;
        if (*r.p == '}') { ++r.p; ok = true; break; }
        if (*r.p++ != ',') break;
    }
    r.ws();
    if (!ok || r.p != r.end || std::strcmp(json_string_value(json_object_get(root, "@type")) ?: "", "stickerSets") ||
        std::strcmp(json_string_value(json_object_get(json_object_get(root, "@extra"), "kind")) ?: "", "sticker_packs") ||
        !json_is_array(json_object_get(root, "sets"))) { json_decref(root); return nullptr; }
    return root;
}

// stickerSet / stickers replies (the open sticker pack, favourites, recent) carry
// outlines, emoji lists and ids that nothing reads, yet Jansson builds and frees
// them at ~2 ms/KB on the Vita. Keep only what applyStickerSlice, the icon path
// and the reaction previews use; skip the rest at grammar level.
namespace td_sets_detail {
inline bool wanted(const std::string& name, std::initializer_list<const char*> keep) {
    for (const char* k : keep) if (name == k) return true;
    return false;
}
// Shapes of the kept objects. A file is the bulk of a sticker (~25 members of
// local/remote state), but only these members are ever read.
enum class Shape { Set, Item, Thumbnail, File, Local, Remote, ChatUpdate, LastMessage, Messages, Message, Content, Media, PhotoSize, AltVideo, MediaFile,
    UserUpdate, User, ProfilePhoto, ChatNew, Chat, ChatPhoto, Preview, PreviewType };
inline Shape child_shape(Shape shape, const std::string& name) {
    switch (shape) {
    case Shape::Set: case Shape::Item:
        if (name == "thumbnail") return Shape::Thumbnail;
        if (name == "sticker") return Shape::File;
        break;
    case Shape::Thumbnail: if (name == "file") return Shape::File; break;
    case Shape::ChatUpdate: if (name == "last_message") return Shape::LastMessage; break;
    case Shape::UserUpdate: if (name == "user") return Shape::User; break;
    case Shape::User: if (name == "profile_photo") return Shape::ProfilePhoto; break;
    case Shape::ProfilePhoto: if (name == "small") return Shape::File; break;
    case Shape::ChatNew: if (name == "chat") return Shape::Chat; break;
    case Shape::Chat:
        if (name == "last_message") return Shape::LastMessage;
        if (name == "photo") return Shape::ChatPhoto;
        break;
    case Shape::ChatPhoto: if (name == "small") return Shape::File; break;
    case Shape::Message: if (name == "content") return Shape::Content; break;
    // Typed media objects: only photo sizes, alternative videos and the file
    // objects inside them are narrowed; every other member stays whole.
    case Shape::Content:
        if (name == "link_preview") return Shape::Preview;
        if (wanted(name, {"photo", "animation", "video", "video_note", "voice_note", "audio", "document", "sticker"})) return Shape::Media;
        break;
    case Shape::Preview: if (name == "type") return Shape::PreviewType; break;
    case Shape::PreviewType:
        if (wanted(name, {"photo", "cover", "thumbnail", "video", "animation", "audio", "document", "video_note"})) return Shape::Media;
        break;
    case Shape::Media:
        if (name == "thumbnail") return Shape::Thumbnail;
        if (wanted(name, {"animation", "video", "voice", "audio", "document", "sticker"})) return Shape::MediaFile;
        break;
    case Shape::PhotoSize: if (name == "photo") return Shape::MediaFile; break;
    case Shape::AltVideo: if (name == "video") return Shape::MediaFile; break;
    case Shape::MediaFile: if (name == "local") return Shape::Local; break;
    case Shape::File:
        if (name == "local") return Shape::Local;
        if (name == "remote") return Shape::Remote;
        break;
    default: break;
    }
    return shape;
}
inline bool keeps(Shape shape, const std::string& name) {
    switch (shape) {
    case Shape::Set: return wanted(name, {"@type", "@extra", "id", "title", "thumbnail", "stickers", "is_installed", "is_archived", "sticker_type"});
    case Shape::Item: return wanted(name, {"@type", "width", "height", "emoji", "format", "full_type", "thumbnail", "sticker", "set_id"});
    case Shape::Thumbnail: return wanted(name, {"@type", "format", "width", "height", "file"});
    case Shape::File: return wanted(name, {"@type", "id", "size", "expected_size", "local", "remote"});
    case Shape::Local: return wanted(name, {"path", "is_downloading_completed", "is_downloading_active", "download_offset", "downloaded_size", "downloaded_prefix_size"});
    case Shape::Remote: return wanted(name, {"unique_id"});
    case Shape::ChatUpdate: return wanted(name, {"@type", "@extra", "chat_id", "positions", "last_message"});
    case Shape::LastMessage: return wanted(name, {"@type", "id", "is_outgoing"});
    case Shape::Messages: return wanted(name, {"@type", "@extra", "total_count", "messages"});
    // Top-level members parseMessage, matchesTopic and the cache read.
    case Shape::Message: return wanted(name, {"@type", "id", "chat_id", "sender_id", "date", "edit_date", "media_album_id",
        "is_outgoing", "is_channel_post", "sending_state", "self_destruct_type", "interaction_info", "topic_id", "reply_to", "forward_info", "content"});
    // Content is read widely, so it is kept except for bulky members nothing reads.
    // "waveform" stays: voice messages draw it (about 100 bytes of 5-bit samples).
    case Shape::Content: return !wanted(name, {"minithumbnail", "outline"});
    case Shape::Preview: return wanted(name, {"url", "site_name", "title", "description", "type"});
    case Shape::PreviewType: return wanted(name, {"@type", "photo", "cover", "thumbnail", "video", "animation", "audio", "document", "video_note"});
    case Shape::Media: return !wanted(name, {"minithumbnail", "outline", "has_stickers", "supports_streaming", "is_animation"});
    case Shape::PhotoSize: return wanted(name, {"type", "width", "height", "photo"});
    case Shape::AltVideo: return wanted(name, {"codec", "width", "height", "video"});
    case Shape::MediaFile: return wanted(name, {"id", "size", "expected_size", "local", "remote"});
    case Shape::UserUpdate: return wanted(name, {"@type", "@extra", "user"});
    // Members parseUser, user_identity and the UI cache read; "type" and "status" stay whole.
    case Shape::User: return wanted(name, {"@type", "id", "first_name", "last_name", "type", "status", "profile_photo", "accent_color_id", "upgraded_gift_colors"});
    case Shape::ProfilePhoto: return wanted(name, {"@type", "id", "small"});
    case Shape::ChatNew: return wanted(name, {"@type", "@extra", "chat"});
    // Members chatFromJson, parseChat and the UI cache (compact_chat) read; "type",
    // "permissions" and "positions" stay whole.
    case Shape::Chat: return wanted(name, {"@type", "id", "type", "title", "photo", "permissions", "positions", "last_message",
        "unread_count", "is_marked_as_unread", "last_read_outbox_message_id", "last_read_inbox_message_id", "accent_color_id", "upgraded_gift_colors"});
    case Shape::ChatPhoto: return wanted(name, {"@type", "small"});
    }
    return false;
}
// Objects of a known shape are rebuilt member by member; anything else kept
// whole (format, full_type, @extra). Returns nullptr on any grammar surprise.
inline json_t* pick_object(td_unused_detail::Reader& r, Shape shape, unsigned depth = 0) {
    if (depth > 16 || r.p == r.end || *r.p++ != '{') return nullptr;
    json_t* out = json_object();
    r.ws();
    if (r.p < r.end && *r.p == '}') { ++r.p; return out; }
    for (;;) {
        std::string name;
        if (!key(r, name)) break;
        const bool want = keeps(shape, name);
        const Shape child = child_shape(shape, name);
        Shape item_shape = Shape::Item;
        bool list = false;
        if (want) {
            if (shape == Shape::Set && name == "stickers") list = true;
            else if (shape == Shape::Messages && name == "messages") { list = true; item_shape = Shape::Message; }
            else if (shape == Shape::Media && name == "sizes") { list = true; item_shape = Shape::PhotoSize; }
            else if (shape == Shape::Content && name == "alternative_videos") { list = true; item_shape = Shape::AltVideo; }
        }
        if (list) {
            if (r.p == r.end || *r.p++ != '[') break;
            auto* items = json_array(); json_object_set_new(out, name.c_str(), items);
            r.ws(); bool closed = false, bad = false;
            if (r.p < r.end && *r.p == ']') { ++r.p; closed = true; }
            while (!closed) {
                r.ws();
                auto* item = pick_object(r, item_shape, depth + 1);
                if (!item) { bad = true; break; }
                json_array_append_new(items, item);
                r.ws();
                if (r.p == r.end) { bad = true; break; }
                if (*r.p == ']') { ++r.p; closed = true; }
                else if (*r.p++ != ',') { bad = true; break; }
            }
            if (bad) break;
        } else if (want && (child != shape || shape == Shape::Content || shape == Shape::Media) && r.p < r.end && *r.p == '{') {
            auto* v = pick_object(r, child, depth + 1);
            if (!v) break;
            json_object_set_new(out, name.c_str(), v);
        } else {
            const char* b = r.p;
            if (!r.value()) break;
            if (want) {
                auto* v = load(b, r.p);
                if (!v) break;
                json_object_set_new(out, name.c_str(), v);
            }
        }
        r.ws();
        if (r.p == r.end) break;
        if (*r.p == '}') { ++r.p; return out; }
        if (*r.p++ != ',') break;
    }
    json_decref(out);
    return nullptr;
}
}
inline json_t* compact_raw_sticker_items(const std::string& text) {
    using namespace td_sets_detail;
    if (text.size() < 2048 || text[0] != '{') return nullptr;
    const std::string_view head(text.data(), 128);
    if (head.find("\"stickerSet\"") == std::string_view::npos && head.find("\"stickers\"") == std::string_view::npos) return nullptr;
    td_unused_detail::Reader r{text.data(), text.data() + text.size()};
    json_t* root = pick_object(r, Shape::Set);
    if (!root) return nullptr;
    r.ws();
    const char* type = json_string_value(json_object_get(root, "@type"));
    if (r.p != r.end || !type || (std::strcmp(type, "stickerSet") && std::strcmp(type, "stickers")) ||
        !json_is_array(json_object_get(root, "stickers"))) { json_decref(root); return nullptr; }
    return root;
}

// updateChatLastMessage / updateChatDraftMessage carry the whole last message
// (7-35 KB each, one per chat at startup), yet the client reads only chat_id,
// positions, last_message.id and last_message.is_outgoing. Anything else
// (including a draft body) is skipped at grammar level.
inline json_t* compact_raw_chat_update(const std::string& text) {
    using namespace td_sets_detail;
    if (text.size() < 1024 || text[0] != '{') return nullptr;
    const std::string_view head(text.data(), 96);
    if (head.find("\"updateChatLastMessage\"") == std::string_view::npos &&
        head.find("\"updateChatDraftMessage\"") == std::string_view::npos) return nullptr;
    td_unused_detail::Reader r{text.data(), text.data() + text.size()};
    json_t* root = pick_object(r, Shape::ChatUpdate);
    if (!root) return nullptr;
    r.ws();
    const char* type = json_string_value(json_object_get(root, "@type"));
    if (r.p != r.end || !type || (std::strcmp(type, "updateChatLastMessage") && std::strcmp(type, "updateChatDraftMessage")) ||
        (!json_is_integer(json_object_get(root, "chat_id")) && !json_is_string(json_object_get(root, "chat_id")))) {
        json_decref(root); return nullptr;
    }
    return root;
}

// messages replies (history pages, 30-165 KB) carry ~25 flags and metadata per
// message that nothing reads. Keep the members parseMessage and the cache use.
inline json_t* compact_raw_messages(const std::string& text) {
    using namespace td_sets_detail;
    if (text.size() < 2048 || text[0] != '{') return nullptr;
    const std::string_view head(text.data(), 64);
    if (head.find("\"@type\":\"messages\"") == std::string_view::npos) return nullptr;
    td_unused_detail::Reader r{text.data(), text.data() + text.size()};
    json_t* root = pick_object(r, Shape::Messages);
    if (!root) return nullptr;
    r.ws();
    const char* type = json_string_value(json_object_get(root, "@type"));
    if (r.p != r.end || !type || std::strcmp(type, "messages") || !json_is_array(json_object_get(root, "messages"))) {
        json_decref(root); return nullptr;
    }
    return root;
}

// updateUser (2-4 KB, ~200 at startup) cost ~4.4 ms each in Jansson on the Parser thread,
// yet the client reads only names, type, status, colors and the small profile photo file.
inline json_t* compact_raw_user_update(const std::string& text) {
    using namespace td_sets_detail;
    if (text.size() < 256 || text[0] != '{') return nullptr;
    const std::string_view head(text.data(), 48);
    if (head.find("\"updateUser\"") == std::string_view::npos) return nullptr;
    td_unused_detail::Reader r{text.data(), text.data() + text.size()};
    json_t* root = pick_object(r, Shape::UserUpdate);
    if (!root) return nullptr;
    r.ws();
    const char* type = json_string_value(json_object_get(root, "@type"));
    json_t* user = json_object_get(root, "user");
    if (r.p != r.end || !type || std::strcmp(type, "updateUser") || !json_is_object(user) ||
        !json_is_integer(json_object_get(user, "id"))) { json_decref(root); return nullptr; }
    return root;
}

// updateNewChat (3-5 KB, ~140 at startup) cost ~7 ms each in Jansson: the full last
// message, notification settings, reactions and the big photo are never read.
inline json_t* compact_raw_new_chat(const std::string& text) {
    using namespace td_sets_detail;
    if (text.size() < 256 || text[0] != '{') return nullptr;
    const std::string_view head(text.data(), 48);
    if (head.find("\"updateNewChat\"") == std::string_view::npos) return nullptr;
    td_unused_detail::Reader r{text.data(), text.data() + text.size()};
    json_t* root = pick_object(r, Shape::ChatNew);
    if (!root) return nullptr;
    r.ws();
    const char* type = json_string_value(json_object_get(root, "@type"));
    json_t* chat = json_object_get(root, "chat");
    if (r.p != r.end || !type || std::strcmp(type, "updateNewChat") || !json_is_object(chat) ||
        !json_is_integer(json_object_get(chat, "id"))) { json_decref(root); return nullptr; }
    return root;
}
