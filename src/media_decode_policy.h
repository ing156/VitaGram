#pragma once
#include <cstdint>
extern "C" {
#include <libavcodec/codec_id.h>
}
namespace media_decode {
// Keep container and codec gates aligned with build-animation-decoder.sh.
inline constexpr const char* video_formats = "mov,mp4,m4a,3gp,3g2,mj2,matroska,webm";
inline constexpr const char* animation_formats = "gif,mov,mp4,m4a,3gp,3g2,mj2,matroska,webm";
inline bool video_codec(AVCodecID id, bool video) {
    return id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_VP8 || id == AV_CODEC_ID_VP9 || (!video && id == AV_CODEC_ID_GIF);
}
}
