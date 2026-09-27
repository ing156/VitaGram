#pragma once
#include <jansson.h>
#include <string>
#include "video_note_frame.h"
inline json_t* video_note_message(const std::string& path, int seconds, int64_t chat, int32_t topic) {
    json_t* note=json_pack("{s:s,s:o,s:n,s:i,s:i}", "@type","inputVideoNote",
        "video_note",json_pack("{s:s,s:s}","@type","inputFileLocal","path",path.c_str()),
        "thumbnail","duration",seconds,"length",video_note::Side);
    json_t* content=json_pack("{s:s,s:o,s:n}","@type","inputMessageVideoNote","video_note",note,"self_destruct_type");
    json_t* request=json_pack("{s:s,s:I,s:o,s:s}","@type","sendMessage","chat_id",(json_int_t)chat,
        "input_message_content",content,"@extra","send_video_note");
    json_object_set_new(request,"topic_id",topic>0 ?
        json_pack("{s:s,s:i}","@type","messageTopicForum","forum_topic_id",topic) : json_null());
    return request;
}
