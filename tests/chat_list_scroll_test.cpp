#include "chat_list_scroll.h"
#include <cassert>
#include <iostream>

int main() {
    for (const int top : {48, 90}) {
        const int viewport = 481-top, height = viewport/(top == 90 ? 8 : 9);
        ChatListScroll scroll;
        scroll.configure(30, height, viewport);
        scroll.drag(-17);
        assert(scroll.offset() == 17);
        const int old_row_y = 5*height-scroll.offset();
        scroll.drag(-7);
        assert(5*height-scroll.offset() == old_row_y-7);
        assert(scroll.hit(height-25) == 0);
        assert(scroll.hit(height-24) == 1);
        assert(scroll.first() == 0);
        assert(scroll.end() == (viewport+24+height-1)/height);
        scroll.drag(9);
        assert(scroll.offset() == 15);
        scroll.drag(1000);
        assert(scroll.offset() == 0);
        scroll.drag(-10000);
        assert(scroll.offset() == scroll.maximum());
        assert(scroll.hit(viewport-1) == 29);
        assert(scroll.hit(-1) == -1 && scroll.hit(viewport) == -1);
        const int bottom = scroll.offset();
        scroll.configure(40, height, viewport); // Appending a page keeps the viewport still.
        assert(scroll.offset() == bottom);
        scroll.reveal(39);
        assert(scroll.offset() == scroll.maximum());
        scroll.reveal(0);
        assert(scroll.offset() == 0);
        scroll.drag(-10000);
        scroll.configure(2, height, viewport);
        assert(scroll.offset() == 0 && scroll.end() == 2);
        assert(scroll.hit(2*height) == -1);
        scroll.configure(0, height, viewport);
        assert(scroll.first() == 0 && scroll.end() == 0 && scroll.hit(0) == -1);
        scroll.configure(40, height, viewport);
        scroll.drag(-100);
        scroll.reset(); // Switching folders starts at the top.
        assert(scroll.offset() == 0);
    }
    std::cout << "Chat list scroll tests passed\n";
}
