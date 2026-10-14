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
    assert(square[0]==180 && square[Side*Side-1]==180 && square[Side*120+120]==180);
    assert(square[Side*Side]==128 && square[FrameBytes-1]==128);
    std::vector<uint32_t> pixels(Side*Side); rgba(square.data(),pixels.data());
    assert((pixels[0]>>24)==0 && (pixels.back()>>24)==0 && (pixels[Side*120+120]>>24)==255);
    // Preview and playback share coverage, preserve edge colour and stay symmetric
    // at both the camera size and decoder's maximum video-note size.
    for (int side : {240,270}) {
        CircleMask mask; mask.resize(side,side);
        std::vector<uint8_t> frame(side*side*4,255);
        mask.apply(frame.data());
        unsigned partial=0;
        for (int y=0;y<side;++y) for (int x=0;x<side;++x) {
            const int i=y*side+x;
            const unsigned a=frame[i*4+3];
            assert(a==frame[(y*side+side-1-x)*4+3]);
            assert(a==frame[((side-1-y)*side+x)*4+3]);
            assert(frame[i*4]==255 && frame[i*4+1]==255 && frame[i*4+2]==255);
            if (a>0 && a<255) ++partial;
            if (side==Side) {
                assert(a==(pixels[i]>>24));
                assert((pixels[i]&0xffffff)==(pixels[Side*120+120]&0xffffff));
            }
        }
        assert(partial>static_cast<unsigned>(side*2));
        assert(frame[3]==0 && frame[(side/2*side+side/2)*4+3]==255);
    }
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
