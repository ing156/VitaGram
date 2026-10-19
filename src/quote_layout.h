#pragma once
#include <algorithm>
#include <vector>

struct QuoteRow {
    int group = -1, width = 0;
    bool first = false, last = false;
};

// Every row of one quote shares its frame width, including after scrolling.
inline void layout_quote_rows(std::vector<QuoteRow>& rows) {
    for (size_t first = 0; first < rows.size();) {
        if (rows[first].group < 0) { ++first; continue; }
        size_t end = first + 1;
        int width = rows[first].width;
        while (end < rows.size() && rows[end].group == rows[first].group) {
            width = std::max(width, rows[end].width);
            ++end;
        }
        for (size_t i = first; i < end; ++i) {
            rows[i].width = width + 36;
            rows[i].first = i == first;
            rows[i].last = i + 1 == end;
        }
        first = end;
    }
}
