#include "video_note_frame.h"
#include "video_note_message.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <iostream>
int main() {
    using namespace video_note;
    std::vector<uint8_t> source(320*240*3/2,128), square(FrameBytes);
    std::fill(source.begin(),source.begin()+320*240,180);
    crop(source.data(),square.data());
    assert(square[0]==16 && square[Side*Side-1]==16 && square[Side*120+120]==180);
    assert(square[Side*Side]==128 && square[FrameBytes-1]==128);
    std::vector<uint32_t> pixels(Side*Side); rgba(square.data(),pixels.data());
    assert(pixels[0]==0 && pixels.back()==0 && (pixels[Side*120+120]>>24)==255);
    for (int topic : {0,42}) {
        json_t* request=video_note_message("/local/note.mp4",60,123456,topic);
        assert(request && !std::strcmp(json_string_value(json_object_get(request,"@type")),"sendMessage"));
        assert(json_integer_value(json_object_get(request,"chat_id"))==123456);
        json_t* target=json_object_get(request,"topic_id");
        if (!topic) assert(json_is_null(target));
        else assert(json_integer_value(json_object_get(target,"forum_topic_id"))==42);
        json_t* content=json_object_get(request,"input_message_content");
        assert(!std::strcmp(json_string_value(json_object_get(content,"@type")),"inputMessageVideoNote"));
        json_t* note=json_object_get(content,"video_note");
        assert(!std::strcmp(json_string_value(json_object_get(note,"@type")),"inputVideoNote"));
        assert(json_integer_value(json_object_get(note,"duration"))==60);
        assert(json_integer_value(json_object_get(note,"length"))==240);
        assert(json_is_null(json_object_get(note,"thumbnail")));
        assert(!std::strcmp(json_string_value(json_object_get(json_object_get(note,"video_note"),"path")),"/local/note.mp4"));
        json_decref(request);
    }
    std::cout << "Circular crop, preview alpha and TDLib video-note payload (chat/topic): PASS\n";
}
