#pragma once
#include <algorithm>
#include <cstdint>

// Download completion and demuxer/decoder readiness are independent milestones.
// In particular a readable prefix does not mean an MP4's tail metadata is available.
struct MediaLoadView {
    bool downloading = false;
    bool controls = false;
    bool waiting = false;
    int percent = -1; // Unknown total size: show byte count and an indeterminate track.
    float fraction = 0;
    int64_t downloaded = 0, size = 0;
    static MediaLoadView make(int64_t size, int64_t downloaded, bool complete, bool decoder_ready) {
        MediaLoadView view;
        view.downloading = !complete;
        view.controls = decoder_ready;
        view.waiting = !decoder_ready;
        view.size = std::max<int64_t>(0, size);
        view.downloaded = std::max<int64_t>(0, downloaded);
        if (view.size > 0) {
            view.fraction = complete ? 1.f : static_cast<float>(std::min(view.downloaded, view.size) / static_cast<double>(view.size));
            view.percent = complete ? 100 : std::min(99, static_cast<int>(view.fraction * 100));
        }
        return view;
    }
};
