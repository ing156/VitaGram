#include "ui_mask.h"
#include "theme_picker.h"
#include <cassert>
#include <chrono>
#include <iostream>

static void reference(uint8_t* pixels, size_t stride, int height, const std::vector<ui_mask::Layer>& layers, const ui_mask::Palette& colors) {
    std::memset(pixels, 0, stride * height);
    for (const auto& layer : layers) for (int y = 0; y < layer.height; ++y) for (int x = 0; x < layer.width; ++x) {
        const unsigned alpha = layer.alpha[y * layer.width + x];
        if (!alpha) continue;
        auto* p = pixels + (y + layer.y) * stride + (x + layer.x) * 4;
        const unsigned back = p[3] * (255 - alpha), total = alpha * 255 + back;
        for (unsigned c = 0; c < 3; ++c)
            p[c] = (uint8_t)((((colors[layer.color] >> (c * 8)) & 255) * alpha * 255 + p[c] * back + total / 2) / total);
        p[3] = (uint8_t)((total + 127) / 255);
    }
}

int main() {
    uint32_t red = 0, blue = 0;
    assert(ui_mask::color("#FF0000", red) && red == 0xFF0000FFu);
    assert(ui_mask::color("#0000ff", blue) && blue == 0xFFFF0000u);
    for (const char* invalid : {"#XYZXYZ", "red", "#0000000", "#00000", "#0000 0"}) {
        uint32_t previous = 123;
        assert(!ui_mask::color(invalid, previous) && previous == 123);
    }
    ui_mask::Palette palette{};
    palette[ui_mask::role("surface")] = red;
    palette[ui_mask::role("border")] = blue;
    const auto original = palette;
    json_error_t error{};
    json_t* object = json_loads("{\"surface\":\"#00FF00\",\"border\":\"broken\"}", 0, &error);
    assert(!ui_mask::palette(object, palette) && palette == original);
    json_decref(object);
    object = json_loads("{\"surface\":\"#00FF00\",\"typo\":\"#FF0000\"}", 0, &error);
    assert(!ui_mask::palette(object, palette) && palette == original);
    json_decref(object);
    object = json_loads("{\"surface\":\"#00FF00\",\"surface\":\"#000000\"}", JSON_REJECT_DUPLICATES, &error);
    assert(!object);

    std::vector<ui_mask::Layer> layers = {
        {1, 0, 2, 1, ui_mask::role("surface"), {255, 0}},
        {1, 0, 2, 1, ui_mask::role("border"), {128, 128}}
    };
    std::array<uint8_t, 32> pixels{}; // stride 16, 3 visible pixels + padding, two rows
    ui_mask::render(pixels.data(), 16, 2, layers, palette);
    assert(pixels[4] == 127 && pixels[5] == 0 && pixels[6] == 128 && pixels[7] == 255);
    assert(pixels[8] == 0 && pixels[10] == 255 && pixels[11] == 128);
    for (int i : {0, 1, 2, 3, 12, 15, 16, 31}) assert(pixels[i] == 0);
    // Same coverage, new input values: no mask rebuild or texture allocation.
    palette[ui_mask::role("surface")] = 0xFF00FF00u;
    ui_mask::render(pixels.data(), 16, 2, layers, palette);
    assert(pixels[4] == 0 && pixels[5] == 127 && pixels[6] == 128 && pixels[7] == 255);
    assert(layers[0].alpha == std::vector<uint8_t>({255, 0}));
    std::vector<ui_mask::Layer> mixed;
    for (int i = 0; i < 5; ++i) {
        ui_mask::Layer layer{0, 0, 64, 64, i, std::vector<uint8_t>(4096)};
        for (size_t x = 0; x < layer.alpha.size(); ++x) layer.alpha[x] = i == 0 ? 255 : (uint8_t)(x * 37 + i * 13);
        mixed.push_back(std::move(layer));
        palette[i] = 0xFF000000u | (unsigned)(i * 93677 + 18827);
    }
    std::vector<uint8_t> actual(64 * 64 * 4), expected(actual.size());
    for (int seed = 0; seed < 20; ++seed) {
        mixed[0].alpha[seed * 13] = (uint8_t)seed;
        reference(expected.data(), 256, 64, mixed, palette);
        ui_mask::render(actual.data(), 256, 64, mixed, palette);
        assert(actual == expected); // Includes fully transparent, opaque and overlapping antialiased pixels.
    }
    auto measure = [&](bool optimized) {
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) {
            if (optimized) ui_mask::render(actual.data(), 256, 64, mixed, palette);
            else reference(expected.data(), 256, 64, mixed, palette);
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    };
    const double previous = measure(false), optimized = measure(true);
    std::cout << "Host compositor, 100x64x64x5: reference " << previous << " ms, optimized " << optimized << " ms\n";
    assert(actual == expected);
    ThemePicker picker;
    picker.open(picker.count - 1);
    picker.move(1);
    assert(picker.focused == 0 && picker.active);
    picker.move(-1);
    assert(picker.focused == picker.count - 1);
    assert(picker.first == std::max(0,picker.count - ThemePicker::visible));
    assert(picker.hit(ThemePicker::left, ThemePicker::top + ThemePicker::header) == picker.first);
    assert(picker.hit(ThemePicker::left + ThemePicker::width, ThemePicker::top + ThemePicker::header) == -1);
    assert(picker.hit(ThemePicker::left, ThemePicker::top + ThemePicker::header - 1) == -1);
    assert(picker.hit(ThemePicker::left, ThemePicker::top + ThemePicker::header + ThemePicker::visible * ThemePicker::row_height) == -1);
    picker.open(0);
    picker.scroll(picker.count);
    assert(picker.first == std::max(0,picker.count - ThemePicker::visible));
    assert(picker.hit(ThemePicker::left + 5, ThemePicker::top + ThemePicker::header + (std::min(picker.count,ThemePicker::visible) - 1) * ThemePicker::row_height) == picker.count - 1);
    picker.scroll(-picker.count);
    assert(picker.first == 0 && picker.focused < ThemePicker::visible);
    for(int count:{1,5,7,16,65}) {
        picker.count=count;picker.open(count-1);
        assert(picker.first==std::max(0,count-ThemePicker::visible));
        if(count<ThemePicker::visible)assert(picker.hit(ThemePicker::left,ThemePicker::top+ThemePicker::header+count*ThemePicker::row_height)==-1);
    }
    picker.close();
    assert(!picker.active);
    picker.open(4);
    assert(picker.focused == 4); // Reopening follows the applied theme, not a canceled selection.
    std::cout << "UI masks: parsing, atomic palette validation, alpha compositing and recoloring passed\n";
}
