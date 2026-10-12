#include "video_thumbnail.h"
#include "vault.h"
#include <cassert>
#include <fstream>
#include <cstdio>

int main(int argc, char** argv) {
    // Complete 1x1 GIF with a red palette entry. Test the actual demuxer/decoder,
    // image worker entry point, malformed input and cooperative cancellation.
    const uint8_t gif[] = {
        'G','I','F','8','9','a',1,0,1,0,0x80,0,0,
        255,0,0,0,0,0,0x2c,0,0,0,0,1,0,1,0,0,2,2,0x44,1,0,0x3b
    };
    std::vector<uint8_t> bytes(std::begin(gif), std::end(gif));
    auto image = video_thumbnail::decode(bytes, [] { return true; });
    assert(image.width == 1 && image.height == 1 && image.rgba.size() == 4);
    assert(image.rgba[0] == 255 && image.rgba[1] == 0 && image.rgba[2] == 0);
    assert(video_thumbnail::decode(bytes, [] { return false; }).rgba.empty());
    assert(video_thumbnail::decode({1,2,3}, [] { return true; }).rgba.empty());
    unsigned checks = 0;
    assert(video_thumbnail::decode(bytes, [&] { return ++checks < 4; }).rgba.empty());
    { std::ofstream file("preview.gif", std::ios::binary); file.write(reinterpret_cast<const char*>(gif), sizeof(gif)); }
    image = decode_image("video-thumb:preview.gif", [] { return true; });
    assert(image.width == 1 && image.height == 1 && image.rgba[0] == 255);
    if (argc == 2) {
        image = decode_image(std::string("video-thumb:") + argv[1], [] { return true; });
        assert(image.width && image.height && image.width <= 320 && image.height <= 320);
        std::printf("MP4 first frame: %ux%u\n", image.width, image.height);
    }
    vault::set_config_path("vault.json"); vault::load();
    assert(vault::enable({0,4,2,6,7,3,1,5,4,0,6,2}) == vault::Result::Ok);
    assert(vault::seal_file("preview.gif") == vault::Convert::Done);
    image = decode_image("video-thumb:preview.gif", [] { return true; });
    assert(image.width == 1 && image.height == 1 && image.rgba[0] == 255);
    vault::lock();
    assert(decode_image("video-thumb:preview.gif", [] { return true; }).rgba.empty());
    std::puts("Video thumbnails: GIF frame, worker path, invalid input and cancellation passed");
}
