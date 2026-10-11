#pragma once
struct SceTouchData {
    unsigned reportNum = 0;
    struct { unsigned id = 0, x = 0, y = 0; } report[2];
};
