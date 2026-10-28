#include "video_sync.h"
#include <cassert>
#include <climits>
int main() {
    assert(!video_audio_ahead(1000000, 1000000, 41708, false));
    assert(!video_audio_ahead(1125124, 1000000, 41708, false));
    assert(video_audio_ahead(1125125, 1000000, 41708, false));
    assert(video_audio_ahead(1100000, 1000000, 41708, true));
    assert(!video_audio_ahead(1060000, 1000000, 41708, true));
    assert(!video_audio_ahead(500000, 1000000, 41708, true));
    assert(!video_audio_ahead(-1, 1000000, 41708, true));
    assert(!video_audio_ahead(5000000, -1, 41708, true));
    assert(!video_audio_ahead(3000000, 1000000, 1000000, false));
    assert(video_audio_ahead(4000001, 1000000, 1000000, false));
    assert(video_audio_ahead(INT64_MAX, 0, INT64_MAX, false));
}
