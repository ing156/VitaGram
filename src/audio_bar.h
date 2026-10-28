#pragma once
#include <algorithm>
#include <cstdint>

namespace audio_bar {
// Defer single shoulder presses until release so entering the player never changes folders.
struct Shoulders {
    unsigned pending = 0;
    bool update(unsigned held, unsigned& pressed, unsigned mask) {
        if ((held&mask) == mask && (pressed&mask)) { pending=0; pressed&=~mask; return true; }
        pending |= pressed&mask;
        pressed = (pressed&~mask) | (pending&~held);
        pending &= held;
        return false;
    }
};
enum Control { Previous, Play, Next, Seek, Volume, Shuffle, Repeat, Speed, Close, Count, None = -1,
               VolumeTrack = Count, Mute };
struct Rect {
    float x, y, w, h;
    bool contains(float px, float py) const { return px >= x && px < x+w && py >= y && py < y+h; }
};
struct Layout {
    float left = 0, bottom = 544;
    static constexpr float height = 72, button = 40;
    float top() const { return bottom-height; }
    float titleX() const { return left+136; }
    float titleWidth() const { return 620-titleX(); }
    Rect panel() const { return {left, top(), 960-left, height}; }
    Rect cell(Control control) const {
        if (control == Seek) return {left+8, top(), 944-left, 16};
        if (control <= Next) return {left+8+int(control)*button, top()+20, button, button};
        return {752+(int(control)-int(Volume))*button, top()+20, button, button};
    }
    Rect popup() const { return {720, top()-88, 232, 80}; }
    Rect volumeTrack() const { return {784, top()-56, 152, 32}; }
    Rect mute() const { return {728, top()-56, 40, 40}; }
    Control hit(float x, float y, bool volume_open) const {
        if (volume_open && volumeTrack().contains(x, y)) return VolumeTrack;
        if (volume_open && mute().contains(x, y)) return Mute;
        for (int i = 0; i < Count; ++i) if (cell(Control(i)).contains(x, y)) return Control(i);
        return None;
    }
    int volumeAt(float x) const { return std::clamp(int((x-volumeTrack().x)*100/volumeTrack().w+0.5f), 0, 100); }
    int64_t seekAt(float x, int64_t duration, int64_t limit) const {
        const auto track = cell(Seek);
        return std::min(std::max<int64_t>(0, limit), (int64_t)(std::clamp((x-track.x)/track.w, 0.f, 1.f)*duration));
    }
};
inline int nextSpeed(int speed, int direction = 1) {
    constexpr int values[] = {50, 75, 100, 125, 150, 175, 200};
    int index = 2;
    for (int i = 0; i < 7; ++i) if (values[i] == speed) index = i;
    return values[(index+direction+7)%7];
}
}
