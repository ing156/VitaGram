#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Straight-alpha coverage, cached per frame size. The feather lies inside the
// circle so scaling cannot expose a hard cut at the square texture boundary.
class CircleMask {
public:
    void resize(int width, int height) {
        if (width == width_ && height == height_) return;
        width_ = width; height_ = height;
        alpha_.resize(static_cast<size_t>(width) * height);
        constexpr float feather = 1.5f;
        const float radius = std::min(width, height) * 0.5f;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const float distance = std::hypot(x + 0.5f - width * 0.5f, y + 0.5f - height * 0.5f);
            const float coverage = std::max(0.f, std::min(1.f, (radius - distance) / feather));
            alpha_[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(std::lround(255.f * coverage));
        }
    }
    uint8_t alpha(size_t pixel) const { return alpha_[pixel]; }
    void apply(uint8_t* rgba) const {
        for (size_t i = 0; i < alpha_.size(); ++i)
            rgba[i * 4 + 3] = (rgba[i * 4 + 3] * alpha_[i] + 127) / 255;
    }
private:
    int width_ = 0, height_ = 0;
    std::vector<uint8_t> alpha_;
};
