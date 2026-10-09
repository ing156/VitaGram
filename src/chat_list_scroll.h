#pragma once

#include <algorithm>

// Pixel offset shared by list rendering, dragging and tap hit testing.
class ChatListScroll {
public:
    void configure(int count, int height, int viewport) {
        count_ = count; height_ = height; viewport_ = viewport;
        offset_ = std::clamp(offset_, 0, maximum());
    }
    void reset() { offset_ = 0; }
    void drag(int finger_delta) {
        offset_ = std::clamp(offset_ - finger_delta, 0, maximum());
    }
    void reveal(int index) {
        if (index < 0 || index >= count_) return;
        if (index * height_ < offset_) offset_ = index * height_;
        if ((index + 1) * height_ > offset_ + viewport_)
            offset_ = (index + 1) * height_ - viewport_;
        offset_ = std::clamp(offset_, 0, maximum());
    }
    int hit(int y) const {
        if (y < 0 || y >= viewport_) return -1;
        const int index = (offset_ + y) / height_;
        return index < count_ ? index : -1;
    }
    int first() const { return offset_ / height_; }
    int end() const { return std::min(count_, (offset_ + viewport_ + height_ - 1) / height_); }
    int offset() const { return offset_; }
    int maximum() const { return std::max(0, count_ * height_ - viewport_); }

private:
    int count_ = 0, height_ = 1, viewport_ = 0, offset_ = 0;
};
