#pragma once

#include <algorithm>
#include <cstdint>

// Keep a partially visible message anchored by ID when history pages change.
class HistoryScroll {
public:
    template<class Id, class Height, class Inset>
    void configure(int count, int viewport, int selected, Id id, Height height, Inset inset) {
        count_ = count; viewport_ = viewport;
        if (!count) { *this = {}; return; }
        int anchor = -1;
        for (int i = 0; i < count; ++i) if (id(i) == first_id_) { anchor = i; break; }
        if (anchor < 0) { first_ = std::clamp(selected, 0, count-1); offset_ = 0; }
        else { first_ = anchor; offset_ += inset(anchor)-first_inset_; }
        move(0, height);
        selected = std::clamp(selected, 0, count-1);
        if (id(selected) != selected_id_) {
            if (selected <= first_) { first_ = selected; offset_ = 0; }
            int bottom = -offset_;
            for (int i = first_; i <= selected; ++i) bottom += height(i);
            if (bottom > viewport_) move(bottom-viewport_, height);
            move(0, height);
            selected_id_ = id(selected);
        }
        first_id_ = id(first_);
        first_inset_ = inset(first_);
    }

    template<class Id, class Height, class Inset>
    void drag(int finger_delta, Id id, Height height, Inset inset) {
        if (!count_) return;
        move(-finger_delta, height);
        first_id_ = id(first_);
        first_inset_ = inset(first_);
    }
    void select(int64_t id) { selected_id_ = id; }
    int first() const { return first_; }
    int offset() const { return offset_; }

private:
    template<class Height>
    void move(int pixels, Height height) {
        offset_ += pixels;
        while (offset_ < 0 && first_ > 0) offset_ += height(--first_);
        while (first_+1 < count_ && offset_ >= height(first_)) offset_ -= height(first_++);
        offset_ = std::max(0, offset_);
        int remaining = -offset_;
        for (int i = first_; i < count_ && remaining < viewport_; ++i) remaining += height(i);
        if (remaining < viewport_) {
            offset_ -= viewport_-remaining;
            while (offset_ < 0 && first_ > 0) offset_ += height(--first_);
            offset_ = std::max(0, offset_);
        }
    }
    int count_ = 0, viewport_ = 0, first_ = 0, offset_ = 0, first_inset_ = 0;
    int64_t first_id_ = 0, selected_id_ = 0;
};
