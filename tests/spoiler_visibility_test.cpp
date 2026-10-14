#include "spoiler_visibility.h"
#include <cassert>
#include <cstdio>
int main() {
    SpoilerVisibility v;
    assert(!v.observe({1,0,false,false}, true));
    v.reveal(1,10); v.reveal(1,11); v.reveal(2,20);
    assert(!v.observe({1,0,false,false}, true)); // Scrolling/post navigation stays in the chat.
    assert(v.revealed(1,10));
    assert(v.observe({2,0,false,false}, true)); // Invalidate sharp textures for the departed chat.
    assert(!v.revealed(1,10) && !v.revealed(1,11) && v.revealed(2,20));
    assert(!v.observe({0,0,false,false}, false)); // Preference off preserves session reveals.
    assert(v.revealed(2,20));
    assert(!v.appLeft(false) && v.revealed(2,20));
    assert(v.appLeft(true) && !v.revealed(2,20));
    assert(!v.appLeft(true)); // No texture invalidation needed for an empty state.
    v.observe({3,8,false,false}, true); v.reveal(3,30);
    assert(v.observe({3,9,false,false}, true) && !v.revealed(3,30));
    v.reveal(3,31);
    assert(v.observe({3,0,true,false}, true) && !v.revealed(3,31)); // Return to forum topic list.
    v.observe({4,0,false,true}, true); v.reveal(4,40);
    assert(v.observe({1,0,false,false}, true) && !v.revealed(4,40)); // Leave a discussion.
    v.reveal(1,10); assert(v.reset() && !v.revealed(1,10));
    v.reveal(0,12); v.reveal(1,0); assert(!v.reset());
    std::puts("Spoilers: chat/topic/discussion exit, app resume, selected reset events and texture invalidation: PASS");
}
