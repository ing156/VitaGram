#include "video_packet_pacer.h"
#include <cassert>
#include <climits>
int main() {
    VideoPacketPacer p;
    assert(p.ready(4000000, 0, 1000000, false, false));
    assert(!p.ready(4000001, 0, 1000000, false, false));
    assert(p.ready(4000001, 1, 1000001, false, false));
    assert(p.ready(5000000, -1, 1000000, false, false));
    assert(!p.ready(5000000, 0, 1000000, true, false));
    assert(p.ready(5000000, 0, 2000000, true, false));
    assert(!p.ready(6000000, 0, 2000000, true, true));
    assert(!p.ready(6000000, 0, 3000000, true, false));
    assert(p.ready(6000000, 0, 5000000, true, false));
    assert(p.ready(6000000, 0, 6000000, false, false));
    assert(!p.ready(7000001, 0, 6000000, false, false));
    VideoPacketPacer intermittent;
    assert(!intermittent.ready(6000000, 0, 1000000, true, false));
    assert(!intermittent.ready(6000000, 0, 2000000, true, false));
    assert(!intermittent.ready(6000000, 100000, 2100000, false, false));
    assert(!intermittent.ready(6000000, 100000, 2200000, true, false));
    assert(intermittent.ready(6000000, 100000, 3200000, true, false));
    assert(!intermittent.ready(6000001, 100000, 3200000, true, false));
    assert(!intermittent.ready(6000000, 100000, 4000000, true, true));
    assert(!intermittent.ready(6000001, 100000, 9000000, true, false));
    assert(intermittent.ready(9000000, 5000000, 9000000, false, false));
    assert(p.ready(INT64_MAX, INT64_MAX - 10, 6000000, false, false));
    assert(p.ready(INT64_MIN, 0, 6000000, false, false));
}
