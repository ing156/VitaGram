#pragma once
#include <algorithm>

struct ChatCalendar {
    int year = 1970, month = 1, day = 1;
    static constexpr int left = 270, top = 184, column = 60, row = 44;
    static constexpr int cell_width = 54, cell_height = 38;
    int cell(int d) const { return firstColumn()+d-1; }
    int x(int d) const { return left+cell(d)%7*column; }
    int y(int d) const { return top+cell(d)/7*row; }
    static bool leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
    static int days(int y, int m) {
        constexpr int count[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        return count[m-1] + (m == 2 && leap(y));
    }
    int key() const { return year*10000 + month*100 + day; }
    int firstColumn() const {
        int y = year - (month < 3);
        constexpr int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
        return (y + y/4 - y/100 + y/400 + t[month-1] + 1 + 6) % 7;
    }
    void moveMonth(int delta, const ChatCalendar& today) {
        int m = year*12 + month-1 + delta;
        m = std::clamp(m, 2013*12+7, today.year*12+today.month-1);
        year = m/12; month = m%12+1;
        day = std::min(day, days(year, month));
    }
    bool enabled(int d, const ChatCalendar& today) const {
        return d >= 1 && d <= days(year, month) && year >= 2013 &&
            (year > 2013 || month >= 8) && year*10000+month*100+d <= today.key();
    }
    int hit(int x, int y) const {
        if (x < left || x >= left+7*column || y < top || y >= top+6*row ||
            (x-left)%column >= cell_width || (y-top)%row >= cell_height) return 0;
        const int d = (y-top)/row*7 + (x-left)/column - firstColumn()+1;
        return d >= 1 && d <= days(year, month) ? d : 0;
    }
};
