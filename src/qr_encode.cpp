#include "qr_encode.h"
#include <algorithm>

namespace {
// Rows: Medium, High. ISO QR block parameters, versions 1-10.
const int kEccPerBlock[2][11] = {
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26},
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28}
};
const int kBlocks[2][11] = {
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5},
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8}
};

int raw_modules(int v) {
    int result = (16 * v + 128) * v + 64;
    if (v >= 2) {
        int align = v / 7 + 2;
        result -= (25 * align - 10) * align - 55;
        if (v >= 7) result -= 36;
    }
    return result;
}
int data_codewords(int v, int level) { return raw_modules(v) / 8 - kEccPerBlock[level][v] * kBlocks[level][v]; }

uint8_t gf_mul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; --i) { z = (z << 1) ^ ((z >> 7) * 0x11D); z ^= ((y >> i) & 1) * x; }
    return (uint8_t)z;
}

std::vector<uint8_t> rs_remainder(const std::vector<uint8_t>& data, int degree) {
    std::vector<uint8_t> divisor(degree, 0);
    divisor[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            divisor[j] = gf_mul(divisor[j], root);
            if (j + 1 < degree) divisor[j] ^= divisor[j + 1];
        }
        root = gf_mul(root, 2);
    }
    std::vector<uint8_t> result(degree, 0);
    for (uint8_t b : data) {
        uint8_t factor = b ^ result[0];
        result.erase(result.begin());
        result.push_back(0);
        for (int j = 0; j < degree; ++j) result[j] ^= gf_mul(divisor[j], factor);
    }
    return result;
}

struct Grid {
    int size;
    std::vector<uint8_t> dark, fixed;
    explicit Grid(int s) : size(s), dark((size_t)s * s, 0), fixed((size_t)s * s, 0) {}
    void set(int x, int y, bool value) { if (x >= 0 && y >= 0 && x < size && y < size) { dark[(size_t)y * size + x] = value; fixed[(size_t)y * size + x] = 1; } }
    bool get(int x, int y) const { return dark[(size_t)y * size + x]; }
};

void draw_finder(Grid& g, int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx) {
            int dist = std::max(std::abs(dx), std::abs(dy));
            g.set(cx + dx, cy + dy, dist != 2 && dist != 4);
        }
}

void draw_alignment(Grid& g, int cx, int cy) {
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) g.set(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
}

void draw_format(Grid& g, int mask, int level) {
    int data = ((level ? 2 : 0) << 3) | mask;  // M = 0, H = 2
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | rem) ^ 0x5412;
    auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
    for (int i = 0; i <= 5; ++i) g.set(8, i, bit(i));
    g.set(8, 7, bit(6)); g.set(8, 8, bit(7)); g.set(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) g.set(14 - i, 8, bit(i));
    for (int i = 0; i < 8; ++i) g.set(g.size - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i) g.set(8, g.size - 15 + i, bit(i));
    g.set(8, g.size - 8, true);
}

void draw_version(Grid& g, int v) {
    if (v < 7) return;
    int rem = v;
    for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    int bits = (v << 12) | rem;
    for (int i = 0; i < 18; ++i) {
        bool b = ((bits >> i) & 1) != 0;
        int a = g.size - 11 + i % 3, c = i / 3;
        g.set(a, c, b); g.set(c, a, b);
    }
}

bool mask_bit(int mask, int x, int y) {
    switch (mask) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return x * y % 2 + x * y % 3 == 0;
        case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
        default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

int penalty(const Grid& g) {
    const int n = g.size;
    int result = 0, dark_count = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (int a = 0; a < n; ++a) {
            int run = 1;
            uint32_t history = 0;  // last 11 modules, 1 = dark
            for (int b = 0; b < n; ++b) {
                bool cell = pass ? g.get(a, b) : g.get(b, a);
                if (pass == 0) dark_count += cell;
                if (b > 0) {
                    bool previous = pass ? g.get(a, b - 1) : g.get(b - 1, a);
                    if (cell == previous) { if (++run == 5) result += 3; else if (run > 5) result += 1; }
                    else run = 1;
                }
                history = ((history << 1) | cell) & 0x7FF;
                if (b >= 10 && (history == 0x05D || history == 0x5D0)) result += 40;  // 1:1:3:1:1 with 4 light modules on a side
            }
        }
    }
    for (int y = 0; y + 1 < n; ++y)
        for (int x = 0; x + 1 < n; ++x) {
            bool c = g.get(x, y);
            if (c == g.get(x + 1, y) && c == g.get(x, y + 1) && c == g.get(x + 1, y + 1)) result += 3;
        }
    int total = n * n;
    int k = (std::abs(dark_count * 20 - total * 10) + total - 1) / total - 1;
    return result + std::max(0, k) * 10;
}
}  // namespace

bool qr_encode(const std::string& text, QrCode& out, QrEcc ecc_level) {
    out = QrCode();
    // Bound length before converting to int or calculating bit counts.
    if (text.size() > 213) return false;
    const int level = ecc_level == QrEcc::High ? 1 : 0;
    int version = 0;
    for (int v = 1; v <= 10; ++v) {
        int needed = 4 + (v <= 9 ? 8 : 16) + 8 * (int)text.size();
        if (needed <= data_codewords(v, level) * 8) { version = v; break; }
    }
    if (!version) return false;
    const int capacity = data_codewords(version, level);

    std::vector<uint8_t> bitstream;
    int bit_count = 0;
    auto put = [&](uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            if (bit_count % 8 == 0) bitstream.push_back(0);
            bitstream.back() |= ((value >> i) & 1) << (7 - bit_count % 8);
            ++bit_count;
        }
    };
    put(0x4, 4);
    put((uint32_t)text.size(), version <= 9 ? 8 : 16);
    for (unsigned char c : text) put(c, 8);
    put(0, std::min(4, capacity * 8 - bit_count));
    bit_count = (bit_count + 7) / 8 * 8;
    bitstream.resize(bit_count / 8);
    for (uint8_t pad = 0xEC; (int)bitstream.size() < capacity; pad ^= 0xEC ^ 0x11) bitstream.push_back(pad);

    // Split into blocks, add error correction, interleave.
    const int blocks = kBlocks[level][version], ecc = kEccPerBlock[level][version], raw = raw_modules(version) / 8;
    const int short_blocks = blocks - raw % blocks, short_len = raw / blocks;
    std::vector<std::vector<uint8_t>> parts;
    for (int i = 0, k = 0; i < blocks; ++i) {
        int length = short_len - ecc + (i < short_blocks ? 0 : 1);
        std::vector<uint8_t> data(bitstream.begin() + k, bitstream.begin() + k + length);
        k += length;
        auto checks = rs_remainder(data, ecc);
        if (i < short_blocks) data.push_back(0);
        data.insert(data.end(), checks.begin(), checks.end());
        parts.push_back(std::move(data));
    }
    std::vector<uint8_t> codewords;
    for (size_t i = 0; i < parts[0].size(); ++i)
        for (int j = 0; j < blocks; ++j)
            if ((int)i != short_len - ecc || j >= short_blocks) codewords.push_back(parts[j][i]);

    // Function patterns.
    const int size = version * 4 + 17;
    Grid g(size);
    for (int i = 0; i < size; ++i) { g.set(6, i, i % 2 == 0); g.set(i, 6, i % 2 == 0); }
    draw_finder(g, 3, 3); draw_finder(g, size - 4, 3); draw_finder(g, 3, size - 4);
    if (version >= 2) {
        int count = version / 7 + 2;
        int step = (version * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
        std::vector<int> positions{6};
        for (int pos = size - 7; (int)positions.size() < count; pos -= step) positions.insert(positions.begin() + 1, pos);
        for (int i = 0; i < count; ++i)
            for (int j = 0; j < count; ++j)
                if (!((i == 0 && j == 0) || (i == 0 && j == count - 1) || (i == count - 1 && j == 0)))
                    draw_alignment(g, positions[i], positions[j]);
    }
    draw_format(g, 0, level);  // reserves the format area
    draw_version(g, version);

    // Zig-zag placement of the data.
    size_t bit = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < size; ++vert)
            for (int j = 0; j < 2; ++j) {
                int x = right - j, y = ((right + 1) & 2) == 0 ? size - 1 - vert : vert;
                size_t index = (size_t)y * size + x;
                if (!g.fixed[index] && bit < codewords.size() * 8) {
                    g.dark[index] = (codewords[bit >> 3] >> (7 - (bit & 7))) & 1;
                    ++bit;
                }
            }
    }

    // Choose the mask with the lowest penalty.
    int best_mask = 0, best_penalty = 1 << 30;
    Grid best = g;
    for (int mask = 0; mask < 8; ++mask) {
        Grid trial = g;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                if (!trial.fixed[(size_t)y * size + x] && mask_bit(mask, x, y)) trial.dark[(size_t)y * size + x] ^= 1;
        draw_format(trial, mask, level);
        int p = penalty(trial);
        if (p < best_penalty) { best_penalty = p; best_mask = mask; best = trial; }
    }
    (void)best_mask;
    out.size = size;
    out.modules = std::move(best.dark);
    return true;
}
