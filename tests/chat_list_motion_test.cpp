#include "chat_list_motion.h"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main() {
    ChatListMotion motion;
    const int64_t t0 = 1000000;
    const int row = 60;

    // The first list appears without motion.
    motion.update({1, 2, 3, 4}, row, 0, t0);
    for (size_t i = 0; i < 4; ++i) assert(near(motion.offset(i, t0), 0));

    // Same ids: nothing moves.
    motion.update({1, 2, 3, 4}, row, 0, t0 + 16000);
    assert(near(motion.offset(0, t0 + 16000), 0));

    // Chat 4 jumps to the top: it starts at its old row; the others shift down by one row.
    const int64_t t1 = t0 + 32000;
    motion.update({4, 1, 2, 3}, row, 0, t1);
    assert(near(motion.offset(0, t1), 3.f * row));
    assert(near(motion.offset(1, t1), -1.f * row));
    assert(near(motion.offset(3, t1), -1.f * row));
    // Ease-out: monotonic towards zero, done after the duration.
    const float mid = motion.offset(0, t1 + ChatListMotion::kDurationUs / 2);
    assert(mid > 0 && mid < 3.f * row / 2);
    assert(near(motion.offset(0, t1 + ChatListMotion::kDurationUs), 0));

    // A new row and rows that kept their place are drawn in place.
    const int64_t t2 = t1 + ChatListMotion::kDurationUs + 16000;
    motion.update({4, 1, 2, 3, 5}, row, 0, t2);
    for (size_t i = 0; i < 5; ++i) assert(near(motion.offset(i, t2), 0));

    // A change mid-slide continues from where the row is drawn now.
    const int64_t t3 = t2 + 16000;
    motion.update({5, 4, 1, 2, 3}, row, 0, t3);
    const int64_t t4 = t3 + 16000;
    const float drawn = motion.offset(1, t4); // chat 4, sliding from row 0 to row 1
    motion.update({4, 5, 1, 2, 3}, row, 0, t4);
    assert(near(motion.offset(0, t4), 1.f * row + drawn));

    // Large jumps are limited to one screen.
    const int64_t t5 = t4 + ChatListMotion::kDurationUs + 16000;
    std::vector<int64_t> many;
    for (int64_t id = 100; id < 140; ++id) many.push_back(id);
    motion.update(many, row, 0, t5);
    motion.update(many, row, 0, t5 + 16000);
    std::vector<int64_t> moved(many);
    std::rotate(moved.begin(), moved.end() - 1, moved.end());
    motion.update(moved, row, 0, t5 + 32000);
    assert(near(motion.offset(0, t5 + 32000), 544.f));

    // Folder change or a list that was not drawn for a while: no slide.
    motion.update({1, 2, 3}, row, 7, t5 + 48000);
    motion.update({3, 2, 1}, row, 7, t5 + 64000);
    assert(!near(motion.offset(0, t5 + 64000), 0));
    motion.update({1, 2, 3}, row, 0, t5 + 80000);
    assert(near(motion.offset(0, t5 + 80000), 0));
    motion.update({3, 2, 1}, row, 0, t5 + 1000000);
    assert(near(motion.offset(0, t5 + 1000000), 0));

    std::puts("chat list motion tests passed");
}
