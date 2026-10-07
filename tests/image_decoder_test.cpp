#include "image_decoder.h"
#include <png.h>
#include <cstdio>
#include <jpeglib.h>
#include <webp/encode.h>
#include <cassert>
#include <atomic>
#include <chrono>
#include <thread>
#include <fstream>
#include <cstdlib>

using namespace std::chrono_literals;
template<class F> void wait_for(F predicate) {
    for (int i=0; i<2000 && !predicate(); ++i) std::this_thread::sleep_for(1ms);
    assert(predicate());
}
int main() {
    const uint8_t pixels[]={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};
    png_image png{}; png.version=PNG_IMAGE_VERSION; png.width=2; png.height=2; png.format=PNG_FORMAT_RGBA;
    assert(png_image_write_to_file(&png,"image.png",0,pixels,0,nullptr));
    jpeg_compress_struct jpeg{}; jpeg_error_mgr error{}; jpeg.err=jpeg_std_error(&error);
    jpeg_create_compress(&jpeg); FILE* file=fopen("image.jpg","wb"); assert(file); jpeg_stdio_dest(&jpeg,file);
    jpeg.image_width=2; jpeg.image_height=2; jpeg.input_components=3; jpeg.in_color_space=JCS_RGB;
    jpeg_set_defaults(&jpeg); jpeg_start_compress(&jpeg,TRUE);
    uint8_t row0[]={255,0,0,0,255,0},row1[]={0,0,255,255,255,255};
    JSAMPROW row=row0; jpeg_write_scanlines(&jpeg,&row,1);row=row1;jpeg_write_scanlines(&jpeg,&row,1);
    jpeg_finish_compress(&jpeg); jpeg_destroy_compress(&jpeg); fclose(file);
    uint8_t* encoded=nullptr;size_t bytes=WebPEncodeLosslessRGBA(pixels,2,2,8,&encoded);assert(bytes);
    {std::ofstream out("image.webp",std::ios::binary);out.write(reinterpret_cast<char*>(encoded),bytes);}WebPFree(encoded);
    for (auto path:{"image.png","image.jpg","image.webp"}) {
        auto image=decode_image(path,[]{return true;});assert(image.width==2 && image.height==2 && image.rgba.size()==16);
        for (size_t i=3;i<16;i+=4)assert(image.rgba[i]==255);
        if (std::string(path)!="image.jpg") assert(image.rgba==std::vector<uint8_t>(pixels,pixels+16));
        assert(decode_image(path,[]{return false;}).rgba.empty());
    }
    jpeg={};jpeg.err=jpeg_std_error(&error);jpeg_create_compress(&jpeg);
    file=fopen("large.jpg","wb");assert(file);jpeg_stdio_dest(&jpeg,file);
    jpeg.image_width=4096;jpeg.image_height=2048;jpeg.input_components=3;jpeg.in_color_space=JCS_RGB;
    jpeg_set_defaults(&jpeg);jpeg_start_compress(&jpeg,TRUE);
    std::vector<uint8_t> jpeg_row(4096*3,255);row=jpeg_row.data();
    while (jpeg.next_scanline<jpeg.image_height) jpeg_write_scanlines(&jpeg,&row,1);
    jpeg_finish_compress(&jpeg);jpeg_destroy_compress(&jpeg);fclose(file);
    auto large=decode_image("large.jpg",[]{return true;});
    assert(large.width==1024 && large.height==512 && large.rgba.size()==1024*512*4);
    {std::ofstream out("broken.jpg",std::ios::binary);out.put(char(0xff));out.put(char(0xd8));out << "corrupt";}
    assert(decode_image("broken.jpg",[]{return true;}).rgba.empty());
    assert(decode_image("missing",[]{return true;}).rgba.empty());
    {std::ofstream out("huge",std::ios::binary);out.seekp(16*1024*1024);out.put('x');}
    assert(decode_image("huge",[]{return true;}).rgba.empty());
    std::vector<uint8_t> wide(2048*2*4,255);
    png.width=2048;png.height=2;
    assert(png_image_write_to_file(&png,"wide.png",0,wide.data(),0,nullptr));
    auto scaled=decode_image("wide.png",[]{return true;});
    assert(scaled.width==1024 && scaled.height==1 && scaled.rgba.size()==4096);
    assert(scaled.rgba.front()==255 && scaled.rgba.back()==255);
    std::atomic<int> entered{0};std::atomic<bool> release{false};
    ImageDecoder worker([&](const std::string&,const std::function<bool()>& proceed){
        ++entered;
        while (!release && proceed()) std::this_thread::sleep_for(1ms);
        return proceed() ? DecodedImage{1,1,{255,0,0,255}} : DecodedImage{};
    });
    worker.request({"old"},false);wait_for([&]{return entered.load()==1;});
    worker.request({"new"},false);release=true;
    std::string path;DecodedImage result;
    wait_for([&]{return worker.take(path,result) || path=="new";});assert(path=="new" && result.rgba.size()==4);
    release=false; worker.request({"logout"},false);wait_for([&]{return entered.load()>=3;});
    worker.clear();release=true;std::this_thread::sleep_for(10ms);assert(!worker.take(path,result));
    release=false; worker.request({"pause"},true);int before=entered;std::this_thread::sleep_for(10ms);assert(entered==before);
    worker.request({"pause"},false);wait_for([&]{return entered.load()>before;});worker.request({"pause"},true);
    worker.stop();assert(!worker.take(path,result));
    std::atomic<int> generation_calls{0};std::atomic<bool> gate{false};
    ImageDecoder generation_worker([&](const std::string&,const std::function<bool()>&){
        int call=++generation_calls;
        while (!gate) std::this_thread::sleep_for(1ms);
        return DecodedImage{1,1,{uint8_t(call),0,0,255}};
    });
    generation_worker.request({"same"},false);wait_for([&]{return generation_calls==1;});
    generation_worker.clear();generation_worker.request({"same"},false);gate=true;
    path.clear();
    wait_for([&]{return generation_worker.take(path,result) || !path.empty();});
    assert(path=="same" && result.rgba.front()==2);
    generation_worker.stop();
    std::puts("Images: PNG/JPEG/WebP pixels, damaged/oversized files, stale screen, clear, pause and stop passed");
}
