#include "quote_layout.h"
#include <cassert>
int main() {
    std::vector<QuoteRow> rows{{-1,0}, {7,80}, {7,160}, {7,40}, {-1,0}, {8,50}, {9,90}};
    layout_quote_rows(rows);
    assert(rows[0].width == 0 && rows[4].width == 0);
    assert(rows[1].width == 196 && rows[2].width == 196 && rows[3].width == 196);
    assert(rows[1].first && !rows[1].last && !rows[2].first && !rows[2].last);
    assert(!rows[3].first && rows[3].last);
    assert(rows[5].first && rows[5].last && rows[5].width == 86);
    assert(rows[6].first && rows[6].last && rows[6].width == 126);
    rows.clear(); layout_quote_rows(rows); assert(rows.empty());
}
