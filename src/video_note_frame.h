#pragma once
#include <cstdint>
#include <algorithm>

namespace video_note {
constexpr int Side = 240, FrameBytes = Side * Side * 3 / 2;
constexpr int SampleRate = 16000, MaxSeconds = 60, FrameInterval = SampleRate / 10;
// Crop the 320x240 YUV420 camera frame to a square and erase the invisible corners.
inline void crop(const uint8_t* camera, uint8_t* out) {
    for (int plane = 0; plane < 3; ++plane) {
        int scale = plane ? 2 : 1, side = Side / scale, width = 320 / scale;
        int source_offset = plane == 0 ? 0 : plane == 1 ? 320*240 : 320*240*5/4;
        int dest_offset = plane == 0 ? 0 : plane == 1 ? Side*Side : Side*Side*5/4;
        for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
            int dx = 2*x + 1 - side, dy = 2*y + 1 - side;
            out[dest_offset + y*side + x] = dx*dx + dy*dy >= side*side ? (plane ? 128 : 16) :
                camera[source_offset + y*width + x + (width-side)/2];
        }
    }
}
inline void rgba(const uint8_t* yuv, uint32_t* out) {
    auto clip = [](int v) { return std::max(0, std::min(255, v)); };
    for (int y = 0; y < Side; ++y) for (int x = 0; x < Side; ++x) {
        int dx = 2*x + 1 - Side, dy = 2*y + 1 - Side;
        if (dx*dx + dy*dy >= Side*Side) { out[y*Side+x] = 0; continue; }
        int c = yuv[y*Side+x] - 16;
        int d = yuv[Side*Side + y/2*(Side/2)+x/2] - 128;
        int e = yuv[Side*Side*5/4 + y/2*(Side/2)+x/2] - 128;
        out[y*Side+x] = 0xff000000u | clip((298*c+409*e+128)>>8) |
            (clip((298*c-100*d-208*e+128)>>8)<<8) | (clip((298*c+516*d+128)>>8)<<16);
    }
}
}
