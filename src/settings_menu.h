#pragma once

namespace settings_menu {
// IDs match the existing settings labels and actions; 33 is the user-colors switch.
inline constexpr int rows[][9] = {
    {-1, -2, -3, -4, -5, -6, -7, -8, 31},
    {0, 28, 3, 33, 17, 18, 30, 4},
    {27, 19, 23},
    {24, 25, 22, 16},
    {29, 7, 1, 2, 32},
    {8, 6},
    {11, 14, 15, 12, 13},
    {10, 9, 26},
    {21, 5, 20},
};
inline constexpr int counts[] = {9, 8, 3, 4, 5, 2, 5, 3, 3};
inline constexpr const char* titles[] = {
    "settings.title", "settings.section.interface", "settings.section.chats",
    "settings.section.controls", "settings.section.data", "settings.section.connection",
    "settings.section.security", "settings.section.updates", "settings.section.about"
};
constexpr bool submenu(int source) { return source < 0 || source == 8 || source == 20 || source == 21 || source == 25; }
constexpr bool vaultDependent(int source) { return source >= 12 && source <= 15; }
constexpr bool rowHit(int x, int y, int top, int height) {
    return x >= 20 && x < 940 && y >= top + 1 && y < top + height - 1;
}
constexpr bool backHit(int x, int y) { return x >= 20 && x < 164 && y >= 34 && y < 64; }
} // namespace settings_menu
