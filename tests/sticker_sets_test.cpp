#include "../src/td_sticker_sets.h"
#include <cassert>
#include <cstdio>
#include <cstring>
int main() {
    std::string pad(5000, 'x');
    std::string t = "{\"@type\":\"stickerSets\",\"total_count\":2,\"sets\":["
        "{\"@type\":\"stickerSetInfo\",\"id\":\"123\",\"title\":\"A\",\"name\":\"a\",\"covers\":["
        "{\"@type\":\"sticker\",\"format\":{\"@type\":\"stickerFormatTgs\"},\"thumbnail\":null,\"sticker\":{\"id\":0,\"local\":{}}},"
        "{\"@type\":\"sticker\",\"thumbnail\":{\"format\":{\"@type\":\"thumbnailFormatWebp\"},\"file\":{\"id\":7}}}],\"pad\":\"" + pad + "\"},"
        "{\"@type\":\"stickerSetInfo\",\"id\":\"9\",\"title\":\"\\u0411\",\"covers\":[]}],"
        "\"@extra\":{\"kind\":\"sticker_packs\",\"chat\":5}}";
    json_t* r = compact_raw_sticker_sets(t);
    assert(r);
    json_t* sets = json_object_get(r, "sets");
    assert(json_array_size(sets) == 2);
    json_t* a = json_array_get(sets, 0);
    assert(json_array_size(json_object_get(a, "covers")) == 1);
    assert(json_integer_value(json_object_get(sticker_pack_preview(a), "id")) == 7);
    assert(!sticker_pack_preview(json_array_get(sets, 1)));
    assert(!json_object_get(a, "pad"));
    json_decref(r);
    assert(!compact_raw_sticker_sets(t.substr(0, t.size() - 3)));

    // stickerSet / stickers: keep what the UI reads, drop outlines/emojis/ids.
    std::string set = "{\"@type\":\"stickerSet\",\"id\":\"77\",\"name\":\"n\",\"title\":\"\\u0411 pack\","
        "\"thumbnail\":{\"format\":{\"@type\":\"thumbnailFormatWebp\"},\"file\":{\"id\":3}},\"thumbnail_outline\":[[1,2,3]],"
        "\"stickers\":[{\"@type\":\"sticker\",\"id\":\"5\",\"set_id\":\"77\",\"width\":512,\"height\":400,\"emoji\":\"\xf0\x9f\x98\x80\","
        "\"format\":{\"@type\":\"stickerFormatWebp\"},\"full_type\":{\"@type\":\"stickerFullTypeCustomEmoji\",\"custom_emoji_id\":\"42\"},"
        "\"outline\":[[{\"@type\":\"vectorPathCommandLine\"}]],\"thumbnail\":{\"format\":{\"@type\":\"thumbnailFormatWebp\"},\"file\":{\"id\":8}},"
        "\"sticker\":{\"id\":9,\"local\":{\"path\":\"/x\"}}}],\"emojis\":[{\"emojis\":[\"a\"]}],\"pad\":\"" + pad + "\","
        "\"@extra\":{\"kind\":\"sticker_items\"}}";
    json_t* s2 = compact_raw_sticker_items(set);
    assert(s2);
    assert(!json_object_get(s2, "emojis") && !json_object_get(s2, "name") && !json_object_get(s2, "pad"));
    assert(json_string_value(json_object_get(s2, "id")));
    json_t* st = json_array_get(json_object_get(s2, "stickers"), 0);
    assert(st && !json_object_get(st, "outline") && !json_object_get(st, "set_id") && !json_object_get(st, "id"));
    assert(json_integer_value(json_object_get(st, "width")) == 512);
    assert(!std::strcmp(json_string_value(json_object_get(st, "emoji")), "\xf0\x9f\x98\x80"));
    assert(json_object_get(st, "full_type") && json_integer_value(json_object_get(json_object_get(st, "sticker"), "id")) == 9);
    assert(json_integer_value(json_object_get(sticker_pack_preview(s2), "id")) == 3);
    json_decref(s2);
    assert(!compact_raw_sticker_items(set.substr(0, set.size() - 3)));
    assert(!compact_raw_sticker_items(t)); // stickerSets stay on their own path
    std::string list = set; list.replace(list.find("stickerSet"), 10, "stickers");
    json_t* s3 = compact_raw_sticker_items(list);
    assert(s3 && !std::strcmp(json_string_value(json_object_get(s3, "@type")), "stickers"));
    json_decref(s3);
    // Real file objects carry ~25 members; only the read ones survive.
    std::string file = "{\"@type\":\"file\",\"id\":9,\"size\":100,\"expected_size\":100,"
        "\"local\":{\"path\":\"/p\",\"can_be_downloaded\":true,\"can_be_deleted\":false,\"is_downloading_active\":false,"
        "\"is_downloading_completed\":true,\"download_offset\":0,\"downloaded_prefix_size\":100,\"downloaded_size\":100},"
        "\"remote\":{\"id\":\"abc\",\"unique_id\":\"U1\",\"is_uploading_active\":false,\"is_uploading_completed\":true,\"uploaded_size\":100}}";
    std::string real = "{\"@type\":\"stickerSet\",\"id\":\"1\",\"title\":\"t\",\"stickers\":[{\"@type\":\"sticker\",\"id\":\"2\",\"width\":512,\"height\":512,"
        "\"emoji\":\"a\",\"format\":{\"@type\":\"stickerFormatWebp\"},\"full_type\":{\"@type\":\"stickerFullTypeRegular\"},"
        "\"thumbnail\":{\"@type\":\"thumbnail\",\"format\":{\"@type\":\"thumbnailFormatWebp\"},\"width\":128,\"height\":128,\"file\":" + file + "},"
        "\"sticker\":" + file + "}],\"pad\":\"" + pad + "\",\"@extra\":{\"kind\":\"sticker_items\"}}";
    json_t* s4 = compact_raw_sticker_items(real);
    assert(s4);
    json_t* it = json_array_get(json_object_get(s4, "stickers"), 0);
    json_t* f = json_object_get(it, "sticker");
    assert(json_object_size(f) == 6 && json_object_size(json_object_get(f, "local")) == 6);
    assert(json_is_integer(json_object_get(json_object_get(f, "local"), "download_offset")));
    assert(json_object_size(json_object_get(f, "remote")) == 1);
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(f, "remote"), "unique_id")), "U1"));
    assert(!json_object_get(json_object_get(f, "local"), "can_be_deleted"));
    assert(json_integer_value(json_object_get(json_object_get(json_object_get(it, "thumbnail"), "file"), "id")) == 9);
    assert(json_integer_value(json_object_get(json_object_get(it, "thumbnail"), "width")) == 128);
    assert(json_integer_value(json_object_get(sticker_pack_preview(s4), "id")) == 9);
    json_decref(s4);
    // Direct scalar loading must match Jansson, including escapes and signs.
    std::string odd = "{\"@type\":\"stickerSet\",\"id\":\"1\",\"title\":\"a\\\"b\\u00e9\",\"stickers\":[{\"@type\":\"sticker\",\"width\":-7,\"height\":0,"
        "\"emoji\":\"\\ud83d\\ude00\",\"format\":{\"@type\":\"stickerFormatWebp\",\"x\":1.5},\"sticker\":{\"id\":123456789012,\"local\":{\"is_downloading_completed\":false,\"path\":\"\"}}}],\"pad\":\"" + pad + "\"}";
    json_t* s5 = compact_raw_sticker_items(odd);
    assert(s5);
    assert(!std::strcmp(json_string_value(json_object_get(s5, "title")), "a\"b\xc3\xa9"));
    json_t* it5 = json_array_get(json_object_get(s5, "stickers"), 0);
    assert(json_integer_value(json_object_get(it5, "width")) == -7 && json_integer_value(json_object_get(it5, "height")) == 0);
    assert(!std::strcmp(json_string_value(json_object_get(it5, "emoji")), "\xf0\x9f\x98\x80"));
    assert(json_real_value(json_object_get(json_object_get(it5, "format"), "x")) == 1.5);
    json_t* f5 = json_object_get(it5, "sticker");
    assert(json_integer_value(json_object_get(f5, "id")) == 123456789012LL);
    assert(json_is_false(json_object_get(json_object_get(f5, "local"), "is_downloading_completed")));
    assert(json_is_string(json_object_get(json_object_get(f5, "local"), "path")));
    json_decref(s5);
    // Chat updates keep only what the client reads from them.
    std::string upd = "{\"@type\":\"updateChatLastMessage\",\"chat_id\":-1001234567890,\"last_message\":{\"@type\":\"message\",\"id\":987654321,"
        "\"sender_id\":{\"@type\":\"messageSenderUser\",\"user_id\":5},\"chat_id\":-1001234567890,\"is_outgoing\":true,\"content\":{\"@type\":\"messageText\","
        "\"text\":{\"text\":\"\\u041f\\u0440\\u0438\\u0432\\u0435\\u0442\",\"entities\":[]}},\"reply_markup\":null,\"pad\":\"" + pad + "\"},"
        "\"positions\":[{\"@type\":\"chatPosition\",\"list\":{\"@type\":\"chatListMain\"},\"order\":\"77\",\"is_pinned\":false}]}";
    json_t* u = compact_raw_chat_update(upd);
    assert(u);
    assert(json_integer_value(json_object_get(u, "chat_id")) == -1001234567890LL);
    json_t* lm = json_object_get(u, "last_message");
    assert(json_integer_value(json_object_get(lm, "id")) == 987654321 && json_is_true(json_object_get(lm, "is_outgoing")));
    assert(!json_object_get(lm, "content") && !json_object_get(lm, "pad") && json_object_size(lm) == 3);
    assert(json_array_size(json_object_get(u, "positions")) == 1);
    json_decref(u);
    std::string draft = upd; draft.replace(draft.find("updateChatLastMessage"), 21, "updateChatDraftMessage");
    json_t* d2 = compact_raw_chat_update(draft); assert(d2); json_decref(d2);
    std::string nolast = "{\"@type\":\"updateChatLastMessage\",\"chat_id\":5,\"positions\":[],\"pad\":\"" + pad + "\"}";
    json_t* d3 = compact_raw_chat_update(nolast); assert(d3 && !json_object_get(d3, "last_message")); json_decref(d3);
    assert(!compact_raw_chat_update(upd.substr(0, upd.size() - 2)));
    assert(!compact_raw_chat_update(t));
    // updateUser keeps the names, type, status and the small profile photo file.
    std::string user = "{\"@type\":\"updateUser\",\"user\":{\"@type\":\"user\",\"id\":777,\"first_name\":\"\\u0418\\u0432\\u0430\\u043d\",\"last_name\":\"P\","
        "\"usernames\":{\"active_usernames\":[\"x\"]},\"phone_number\":\"\",\"status\":{\"@type\":\"userStatusOffline\",\"was_online\":5},"
        "\"profile_photo\":{\"@type\":\"profilePhoto\",\"id\":\"9\",\"small\":{\"@type\":\"file\",\"id\":42,\"size\":10,\"expected_size\":10,"
        "\"local\":{\"@type\":\"localFile\",\"path\":\"ux0:a.jpg\",\"is_downloading_completed\":true,\"can_be_deleted\":true},"
        "\"remote\":{\"@type\":\"remoteFile\",\"id\":\"AAA\",\"unique_id\":\"U\"}},\"big\":{\"@type\":\"file\",\"id\":43},"
        "\"minithumbnail\":{\"data\":\"" + pad + "\"},\"has_animation\":false},\"emoji_status\":null,\"is_contact\":true,"
        "\"type\":{\"@type\":\"userTypeBot\",\"can_join_groups\":true},\"language_code\":\"ru\"}}";
    json_t* cu = compact_raw_user_update(user);
    assert(cu);
    json_t* cuser = json_object_get(cu, "user");
    assert(json_integer_value(json_object_get(cuser, "id")) == 777);
    assert(!std::strcmp(json_string_value(json_object_get(cuser, "first_name")), "\xd0\x98\xd0\xb2\xd0\xb0\xd0\xbd"));
    assert(!json_object_get(cuser, "usernames") && !json_object_get(cuser, "is_contact") && !json_object_get(cuser, "language_code"));
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(cuser, "type"), "@type")), "userTypeBot"));
    assert(json_integer_value(json_object_get(json_object_get(cuser, "status"), "was_online")) == 5);
    json_t* cphoto = json_object_get(cuser, "profile_photo");
    assert(!json_object_get(cphoto, "big") && !json_object_get(cphoto, "minithumbnail"));
    json_t* csmall = json_object_get(cphoto, "small");
    assert(!std::strcmp(json_string_value(json_object_get(csmall, "@type")), "file") && json_integer_value(json_object_get(csmall, "id")) == 42);
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(csmall, "local"), "path")), "ux0:a.jpg"));
    assert(json_is_true(json_object_get(json_object_get(csmall, "local"), "is_downloading_completed")));
    assert(!std::strcmp(json_string_value(json_object_get(json_object_get(csmall, "remote"), "unique_id")), "U"));
    json_decref(cu);
    std::string nophoto = "{\"@type\":\"updateUser\",\"user\":{\"@type\":\"user\",\"id\":8,\"first_name\":\"A\",\"profile_photo\":null,\"pad\":\"" + pad + "\"}}";
    json_t* np = compact_raw_user_update(nophoto); assert(np && json_is_null(json_object_get(json_object_get(np, "user"), "profile_photo"))); json_decref(np);
    std::string status = "{\"@type\":\"updateUserStatus\",\"user_id\":8,\"status\":{\"@type\":\"userStatusOnline\",\"expires\":1},\"pad\":\"" + pad + "\"}";
    assert(!compact_raw_user_update(status));
    assert(!compact_raw_user_update(user.substr(0, user.size() - 2)));
    // updateNewChat keeps what the chat list and the UI cache read.
    std::string newchat = "{\"@type\":\"updateNewChat\",\"chat\":{\"@type\":\"chat\",\"id\":-1009,\"type\":{\"@type\":\"chatTypeSupergroup\",\"supergroup_id\":9,\"is_channel\":true},"
        "\"title\":\"\\u041a\\u0430\\u043d\\u0430\\u043b\",\"photo\":{\"@type\":\"chatPhotoInfo\",\"small\":{\"@type\":\"file\",\"id\":77,\"size\":5,"
        "\"local\":{\"path\":\"ux0:c.jpg\",\"is_downloading_completed\":true},\"remote\":{\"id\":\"R\",\"unique_id\":\"Q\"}},\"big\":{\"@type\":\"file\",\"id\":78},"
        "\"minithumbnail\":{\"data\":\"" + pad + "\"}},\"permissions\":{\"can_send_basic_messages\":true},"
        "\"last_message\":{\"@type\":\"message\",\"id\":555,\"is_outgoing\":false,\"content\":{\"@type\":\"messageText\",\"text\":{\"text\":\"x\"}}},"
        "\"positions\":[{\"@type\":\"chatPosition\",\"list\":{\"@type\":\"chatListMain\"},\"order\":\"99\",\"is_pinned\":false}],"
        "\"unread_count\":3,\"is_marked_as_unread\":true,\"last_read_outbox_message_id\":550,\"notification_settings\":{\"mute_for\":0},"
        "\"available_reactions\":{\"@type\":\"chatAvailableReactionsAll\"},\"draft_message\":null}}";
    json_t* nc = compact_raw_new_chat(newchat);
    assert(nc);
    json_t* cchat = json_object_get(nc, "chat");
    assert(json_integer_value(json_object_get(cchat, "id")) == -1009);
    assert(json_is_true(json_object_get(json_object_get(cchat, "type"), "is_channel")));
    assert(json_integer_value(json_object_get(cchat, "unread_count")) == 3 && json_is_true(json_object_get(cchat, "is_marked_as_unread")));
    assert(json_integer_value(json_object_get(cchat, "last_read_outbox_message_id")) == 550);
    assert(json_is_true(json_object_get(json_object_get(cchat, "permissions"), "can_send_basic_messages")));
    assert(json_array_size(json_object_get(cchat, "positions")) == 1);
    json_t* clast = json_object_get(cchat, "last_message");
    assert(json_integer_value(json_object_get(clast, "id")) == 555 && !json_object_get(clast, "content"));
    json_t* cphoto2 = json_object_get(cchat, "photo");
    assert(!json_object_get(cphoto2, "big") && !json_object_get(cphoto2, "minithumbnail"));
    assert(json_integer_value(json_object_get(json_object_get(cphoto2, "small"), "id")) == 77);
    assert(!json_object_get(cchat, "notification_settings") && !json_object_get(cchat, "available_reactions") && !json_object_get(cchat, "draft_message"));
    json_decref(nc);
    assert(!compact_raw_new_chat(newchat.substr(0, newchat.size() - 2)));
    assert(!compact_raw_new_chat(user));
    // messages replies: only the members the client reads survive.
    std::string msg = "{\"@type\":\"message\",\"id\":11,\"sender_id\":{\"@type\":\"messageSenderUser\",\"user_id\":5},\"chat_id\":-100,"
        "\"is_outgoing\":false,\"can_be_edited\":false,\"can_be_forwarded\":true,\"reply_to\":null,\"forward_info\":{\"x\":1},\"date\":1700000000,"
        "\"interaction_info\":{\"reactions\":{\"reactions\":[]}},\"content\":{\"@type\":\"messagePhoto\",\"photo\":{\"minithumbnail\":{\"data\":\"AAAA\"},"
        "\"sizes\":[{\"width\":90,\"height\":60,\"photo\":{\"id\":4,\"local\":{\"path\":\"\"}}}]},\"caption\":{\"text\":\"\\u041f\",\"entities\":[]},\"link_preview\":{\"url\":\"u\"}}}";
    std::string hist = "{\"@type\":\"messages\",\"total_count\":2,\"messages\":[" + msg + "," + msg + "],\"pad\":\"" + pad + "\",\"@extra\":{\"kind\":\"history\"}}";
    json_t* h = compact_raw_messages(hist);
    assert(h && json_array_size(json_object_get(h, "messages")) == 2 && !json_object_get(h, "pad"));
    json_t* m0 = json_array_get(json_object_get(h, "messages"), 0);
    assert(json_integer_value(json_object_get(m0, "id")) == 11 && json_integer_value(json_object_get(m0, "chat_id")) == -100);
    // Unread members go; reply_to and forward_info stay - parseMessage reads them for reply quotes,
    // "forwarded" headers and comment threads.
    assert(!json_object_get(m0, "can_be_edited") && !json_object_get(m0, "can_be_forwarded"));
    assert(json_object_get(m0, "reply_to") && json_is_object(json_object_get(m0, "forward_info")));
    json_t* c0 = json_object_get(m0, "content");
    assert(!json_object_get(c0, "link_preview") && !json_object_get(json_object_get(c0, "photo"), "minithumbnail"));
    assert(json_array_size(json_object_get(json_object_get(c0, "photo"), "sizes")) == 1 && json_object_get(c0, "caption"));
    json_decref(h);
    // Typed media: file objects keep only what is read; alternatives and sizes are narrowed.
    std::string vid = "{\"@type\":\"message\",\"id\":12,\"chat_id\":-100,\"date\":1,\"content\":{\"@type\":\"messageVideo\","
        "\"video\":{\"duration\":5,\"width\":640,\"height\":360,\"supports_streaming\":true,\"minithumbnail\":{\"data\":\"A\"},"
        "\"thumbnail\":{\"format\":{\"@type\":\"thumbnailFormatJpeg\"},\"width\":10,\"height\":10,\"file\":{\"id\":8,\"junk\":1,\"local\":{\"path\":\"p\",\"can_be_deleted\":true}}},"
        "\"video\":{\"id\":9,\"size\":100,\"junk\":2,\"local\":{\"path\":\"v\",\"is_downloading_completed\":true,\"can_be_deleted\":true},\"remote\":{\"id\":\"r\",\"unique_id\":\"u\"}}},"
        "\"alternative_videos\":[{\"id\":3,\"codec\":\"h264\",\"width\":320,\"height\":180,\"hls_file\":{\"id\":1},\"video\":{\"id\":10,\"size\":50,\"junk\":3,\"local\":{\"path\":\"\"}}}]}}";
    std::string vhist = "{\"@type\":\"messages\",\"total_count\":1,\"messages\":[" + vid + "],\"pad\":\"" + pad + "\"}";
    json_t* vh = compact_raw_messages(vhist); assert(vh);
    json_t* vc = json_object_get(json_array_get(json_object_get(vh, "messages"), 0), "content");
    json_t* video = json_object_get(vc, "video");
    assert(json_integer_value(json_object_get(video, "duration")) == 5 && !json_object_get(video, "minithumbnail") && !json_object_get(video, "supports_streaming"));
    json_t* vf = json_object_get(video, "video");
    assert(json_integer_value(json_object_get(vf, "id")) == 9 && !json_object_get(vf, "junk") && json_object_get(vf, "remote"));
    assert(!json_object_get(json_object_get(vf, "local"), "can_be_deleted") && json_is_true(json_object_get(json_object_get(vf, "local"), "is_downloading_completed")));
    assert(!json_object_get(json_object_get(json_object_get(video, "thumbnail"), "file"), "junk"));
    json_t* alt = json_array_get(json_object_get(vc, "alternative_videos"), 0);
    assert(!json_object_get(alt, "hls_file") && !json_object_get(alt, "id") && !json_object_get(json_object_get(alt, "video"), "junk") &&
           json_integer_value(json_object_get(json_object_get(alt, "video"), "id")) == 10);
    json_decref(vh);
    assert(!compact_raw_messages(hist.substr(0, hist.size() - 2)));
    assert(!compact_raw_messages(t));
    std::string nullmsg = hist; nullmsg.replace(nullmsg.find("[{"), 2, "[null,{");
    assert(!compact_raw_messages(nullmsg));
    std::puts("sticker sets raw compaction passed");
}
