#include "history_scroll.h"
#include <cassert>
#include <iostream>
#include <numeric>
#include <vector>

int main() {
    for (int viewport : {180, 406, 464}) {
        std::vector<int> heights{62, 138, 90, 220, 74, 110, 86, 160};
        std::vector<int64_t> ids{1, 2, 3, 4, 5, 6, 7, 8};
        auto id = [&](int i) { return ids[i]; };
        auto inset = [](int) { return 0; };
        auto height = [&](int i) { return heights[i]; };
        auto absolute = [&](const HistoryScroll& s) {
            return std::accumulate(heights.begin(), heights.begin()+s.first(), s.offset());
        };
        const int maximum = std::accumulate(heights.begin(), heights.end(), 0)-viewport;
        HistoryScroll scroll;
        scroll.configure(8, viewport, 0, id, height, inset);
        int expected = 0;
        for (int delta : {-1, -13, -32, -87, 7, -320, 19, -10000, 1, 10000}) {
            scroll.drag(delta, id, height, inset);
            expected = std::clamp(expected-delta, 0, maximum);
            assert(absolute(scroll) == expected);
            scroll.configure(8, viewport, 0, id, height, inset);
            assert(absolute(scroll) == expected); // An unchanged selection cannot undo a drag.
        }
        scroll.configure(8, viewport, 7, id, height, inset);
        assert(absolute(scroll) == maximum);
        scroll.configure(8, viewport, 0, id, height, inset);
        assert(absolute(scroll) == 0);
        scroll.drag(-173, id, height, inset);
        const int64_t anchor = id(scroll.first());
        const int offset = scroll.offset();
        ids.insert(ids.begin(), 99); heights.insert(heights.begin(), 126);
        scroll.configure(9, viewport, 1, id, height, inset);
        assert(id(scroll.first()) == anchor && scroll.offset() == offset);
        ids.push_back(100); heights.push_back(84);
        scroll.configure(10, viewport, 1, id, height, inset);
        assert(id(scroll.first()) == anchor && scroll.offset() == offset);
        scroll.select(id(4));
        scroll.configure(10, viewport, 4, id, height, inset);
        assert(id(scroll.first()) == anchor && scroll.offset() == offset);
        scroll.configure(1, viewport, 0, id, height, inset);
        assert(scroll.first() == 0 && scroll.offset() == 0);
        scroll.configure(0, viewport, -1, id, height, inset);
        assert(scroll.first() == 0 && scroll.offset() == 0);
        heights[0] = viewport+100; // A card taller than the pane is still draggable.
        scroll.configure(1, viewport, 0, id, height, inset);
        assert(scroll.offset() == 100);
        scroll.drag(1, id, height, inset);
        assert(scroll.offset() == 99);
    }
    // Prepending the same day removes the old first message's date chip.
    std::vector<int64_t> ids{1, 2, 3, 4};
    auto id = [&](int i) { return ids[i]; };
    auto inset = [](int i) { return i == 0 ? 26 : 0; };
    auto height = [&](int i) { return 100+inset(i); };
    HistoryScroll scroll;
    scroll.configure(4, 200, 0, id, height, inset);
    scroll.drag(-40, id, height, inset);
    const int body_top = inset(scroll.first())-scroll.offset();
    ids.insert(ids.begin(), 99);
    scroll.configure(5, 200, 1, id, height, inset);
    assert(id(scroll.first()) == 1);
    assert(inset(scroll.first())-scroll.offset() == body_top);
    std::cout << "History pixel scroll tests passed\n";
}
