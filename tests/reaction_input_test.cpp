#include "reaction_input.h"
#include "picker_input.h"
#include <cassert>
#include <cstdio>

int main() {
    ReactionPress press;
    auto sample = [&](int64_t now, bool down, bool fresh = false, bool context = true,
                      bool cancel = false, int64_t chat = 42, int64_t message = 55, int topic = 0) {
        return press.update(context, down, fresh, cancel, chat, message, topic, now);
    };
    auto result = sample(1000, true, true);
    assert(result.consume && !result.short_press && !result.long_press);
    result = sample(101000, false);
    assert(result.consume && result.short_press && !result.long_press);
    assert(!sample(102000, false).consume);
    sample(200000, true, true);
    sample(400000, true);
    sample(600000, true);
    assert(!sample(749999, true).long_press);
    assert(sample(750000, true).long_press);
    assert(!sample(800000, true).long_press);
    result = sample(850000, false, false, false);
    assert(result.consume && !result.short_press && !result.long_press);
    // No action survives changed selection/chat/topic, overlays, other input or a resume gap.
    for (int reason = 0; reason < 7; ++reason) {
        press = ReactionPress{};
        sample(1000, true, true);
        sample(reason == 6 ? 400000 : 101000, true, false, reason != 0, reason == 1,
               reason == 2 ? 43 : 42, reason == 3 ? 56 : 55, reason == 4 ? 1 : 0);
        if (reason == 5) sample(151000, true, false, false);
        result = sample(201000, false);
        assert(!result.short_press && !result.long_press);
    }
    press = ReactionPress{};
    assert(!sample(1000, true, true, false).consume);
    assert(!sample(101000, true).consume); // holding on entry doesn't start a gesture
    assert(!sample(201000, false).short_press);
    // Releasing exactly at the threshold still opens the picker, without a short action.
    sample(300000, true, true);
    sample(500000, true);
    sample(700000, true);
    result = sample(850000, false);
    assert(result.long_press && !result.short_press);
    for (int top = 0; top <= 8; ++top) {
        ReactionGrid grid{70, top};
        for (int index = 0; index < grid.count; ++index) {
            assert(grid.index(grid.slot(index)) == index);
            const int cell = grid.slot(index), x = grid.x(cell), y = grid.y(cell);
            assert(grid.hit(x, y, index) == index);
            assert(grid.hit(x + grid.width - 1, y + grid.height - 1, index) == index);
            assert(grid.hit(x + grid.width, y, index) == -1);
            assert(grid.hit(x, y + grid.height, index) == -1);
            assert(grid.hit(grid.left - 1, y, index) == -1);
            assert(grid.hit(x, grid.y_top - 1, index) == -1);
            for (int direction : {-8, -1, 1, 8}) {
                const int moved = grid.move(index, direction);
                assert(moved >= 0 && moved < grid.count);
                assert(grid.index(grid.slot(moved)) == moved);
            }
        }
        if (top) {
            assert(grid.slot(top) == 8);
            for (int slot = top; slot < 8; ++slot) {
                assert(grid.index(slot) == -1);
                assert(grid.hit(grid.x(slot), grid.y(slot), 0) == -1);
            }
            assert(grid.move(top, -8) == 0);
        }
        assert(grid.index(grid.slot(69) + 1) == -1);
    }
    assert((ReactionGrid{0, 0}.index(0) == -1));
    assert((ReactionGrid{0, 0}.hit(32, 130, 0) == -1));
    assert((ReactionGrid{70, 8}.hit(32, 418, 0) == -1));
    assert((ReactionGrid{70, 8}.hit(928, 130, 0) == -1));
    assert((ReactionGrid{70, 8}.hit(704, 130, 69) == -1)); // empty cell on the final page
    assert((ReactionGrid{3, 3}.move(2, 8) == 2));
    for (bool touchscreen : {false, true}) {
        PickerPress picker;
        PickerPress::Target target{42, 3, 1, 7, 55};
        auto sticker_sample = [&](int64_t now, bool held, bool fresh = false, bool cancel = false,
                                  bool context = true, int hit = 7) {
            return picker.update(context, target, !touchscreen && held, !touchscreen && fresh,
                touchscreen && held, touchscreen && fresh, hit, cancel, now);
        };
        auto pick = sticker_sample(1000, true, true);
        assert(!pick.send && !pick.preview);
        assert(touchscreen ? pick.consume_touch : pick.consume_cross);
        assert(sticker_sample(101000, false).send);
        assert(!sticker_sample(102000, false).send);
        sticker_sample(200000, true, true);
        sticker_sample(400000, true);
        sticker_sample(600000, true);
        pick = sticker_sample(750000, true);
        assert(pick.preview && !pick.send);
        assert(sticker_sample(800000, true).preview);
        pick = sticker_sample(850000, false);
        assert(!pick.preview && !pick.send);
        // The pack field also identifies GIF search generations, including a new result at the same index.
        // Every target field, navigation, overlays and leaving the touched cell cancel a hold.
        for (int reason = 0; reason < 8; ++reason) {
            picker = PickerPress{};
            target = {42, 3, 1, 7, 55};
            sticker_sample(1000, true, true);
            sticker_sample(201000, true);
            sticker_sample(401000, true);
            assert(sticker_sample(551000, true).preview);
            if (reason == 0) ++target.chat;
            if (reason == 1) ++target.topic;
            if (reason == 2) ++target.pack;
            if (reason == 3) ++target.index;
            if (reason == 4) ++target.file;
            pick = sticker_sample(601000, true, false, reason == 5, reason != 6,
                reason == 7 && touchscreen ? -1 : 7);
            if (reason != 7 || touchscreen) assert(!pick.preview && !pick.send);
            assert(!sticker_sample(701000, false).send);
        }
        picker = PickerPress{};
        target = {42, 3, 1, 7, 55};
        sticker_sample(1000, true, true);
        sticker_sample(201000, true);
        sticker_sample(401000, true);
        pick = sticker_sample(551000, false); // threshold reached on release
        assert(!pick.preview && !pick.send);
        assert(!sticker_sample(601000, true).preview); // holding on entry cannot start a preview
    }
    std::puts("Reactions, stickers and GIFs: short/long presses, touch preview, cancellation and grid navigation passed");
}
