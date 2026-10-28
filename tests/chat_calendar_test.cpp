#include "chat_calendar.h"
#include <cassert>
int main() {
    assert(ChatCalendar::days(2000, 2) == 29);
    assert(ChatCalendar::days(2100, 2) == 28);
    ChatCalendar today{2026,10,28}, c{2026,10,28};
    assert(c.firstColumn() == 3);
    assert(c.hit(270,184) == 0);
    assert(c.hit(450,184) == 1);
    assert(c.hit(504,184) == 0);
    assert(c.hit(450,222) == 0);
    assert(c.hit(690,184) == 0);
    assert(c.hit(450,448) == 0);
    assert(c.enabled(28,today) && !c.enabled(29,today));
    c.moveMonth(1,today); assert(c.month == 10);
    c = {2024,3,31}; c.moveMonth(-1,today); assert(c.day == 29);
    c = {2013,8,1}; c.moveMonth(-1,today); assert(c.month == 8 && c.year == 2013);
    for (int y = 2013; y <= 2026; ++y) for (int m = 1; m <= 12; ++m) {
        c = {y,m,1};
        for (int d = 1; d <= ChatCalendar::days(y,m); ++d) {
            int cell = c.firstColumn()+d-1;
            assert(c.hit(270+cell%7*60+27,184+cell/7*44+19) == d);
        }
    }
}
