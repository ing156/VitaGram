#pragma once
#include <algorithm>
#include <cstdint>

class ReactionPress {
public:
    struct Result { bool consume = false, short_press = false, long_press = false; };
    Result update(bool context, bool down, bool fresh, bool cancel,
                  int64_t chat, int64_t message, int32_t topic, int64_t now) {
        if (fresh && context && !cancel) {
            tracking_ = true; pending_ = true; since_ = now;
            chat_ = chat; message_ = message; topic_ = topic;
        }
        Result result{tracking_};
        if (!tracking_) return result;
        if (!context || cancel || chat != chat_ || message != message_ || topic != topic_ ||
            (last_ && (now < last_ || now - last_ > 250000))) pending_ = false;
        last_ = now;
        if (pending_ && now - since_ >= 550000) {
            result.long_press = true;
            pending_ = false;
        }
        if (!down) {
            result.short_press = pending_;
            tracking_ = pending_ = false;
            last_ = 0;
        }
        return result;
    }
private:
    bool tracking_ = false, pending_ = false;
    int64_t since_ = 0, last_ = 0, chat_ = 0, message_ = 0;
    int32_t topic_ = 0;
};

// Keep the preferred reactions in their own first row, even when it isn't full.
struct ReactionGrid {
    static constexpr int left = 32, y_top = 130, columns = 8, page_size = 32;
    static constexpr int step_x = 112, step_y = 72, width = 104, height = 64;
    int count, top;
    int page(int current) const { return slot(current) / page_size * page_size; }
    static int x(int cell) { return left + cell % columns * step_x; }
    static int y(int cell) { return y_top + cell % page_size / columns * step_y; }
    int hit(int px, int py, int current) const {
        px -= left; py -= y_top;
        if (px < 0 || py < 0 || px >= columns * step_x || py >= page_size / columns * step_y ||
            px % step_x >= width || py % step_y >= height) return -1;
        return index(page(current) + py / step_y * columns + px / step_x);
    }
    int slot(int index) const { return index < top || !top ? index : index + 8 - top; }
    int index(int slot) const {
        if (slot < 0 || (top && slot >= top && slot < 8)) return -1;
        const int value = top && slot >= 8 ? slot - 8 + top : slot;
        return value < count ? value : -1;
    }
    int move(int current, int direction) const {
        if (!count) return 0;
        if (direction == -1 || direction == 1) return std::clamp(current + direction, 0, count - 1);
        const int target = std::clamp(slot(current) + direction, 0, slot(count - 1));
        const int value = index(target);
        return value >= 0 ? value : std::max(0, top - 1);
    }
};
