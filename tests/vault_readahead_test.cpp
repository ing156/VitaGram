#include "vault.h"
#include "vault_readahead.h"
#include <unistd.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>

using namespace vault;

static std::string dir;

static std::string blob(size_t n) { std::mt19937_64 r(77); std::string s(n, 0); for (auto& c : s) c = (char)r(); return s; }

static std::unique_ptr<Reader> open_reader(const std::string& path) {
    auto r = std::make_unique<Reader>();
    assert(r->open(path));
    return r;
}

int main() {
    char tmpl[] = "/tmp/vgrahXXXXXX";
    dir = mkdtemp(tmpl);
    set_config_path(dir + "/vault.json");
    load();
    assert(enable(Pin{0, 4, 2, 6, 7, 3, 1, 5, 4, 0, 6, 2}) == Result::Ok);

    const size_t n = 5 * 1024 * 1024 + 123; // many chunks, longer than the 8-block ring
    const std::string data = blob(n), path = dir + "/big";
    { Writer w; assert(w.open(path) && w.write(data.data(), data.size()) && w.close()); }

    // Straight sequential read with odd request sizes.
    {
        ReadAhead ahead; assert(ahead.open(open_reader(path)) && ahead.size() == (int64_t)n);
        std::string out; std::mt19937 rng(5); char buf[100000];
        for (;;) {
            int64_t got = ahead.read(buf, 1 + rng() % 100000);
            assert(got >= 0);
            if (got == 0) break;
            out.append(buf, (size_t)got);
        }
        assert(out == data);
        assert(ahead.read(buf, 10) == 0);
    }
    // Random jumps (forward in the buffer, outside it, backwards, from the end) always match the file.
    {
        ReadAhead ahead; assert(ahead.open(open_reader(path)));
        std::mt19937 rng(9); std::string buf(200000, 0);
        for (int i = 0; i < 300; ++i) {
            int64_t target;
            switch (rng() % 4) {
                case 0: target = (int64_t)(rng() % n); break;
                case 1: target = std::min<int64_t>(n, 1 + (int64_t)(rng() % 150000)); break;     // near the start
                case 2: target = -1; break;                                                         // relative small step
                default: target = (int64_t)n - 1 - (int64_t)(rng() % 5000); break;
            }
            int64_t at;
            if (target < 0) at = ahead.seek(1000, SEEK_CUR);
            else at = ahead.seek(target, SEEK_SET);
            assert(at >= 0);
            if (at >= (int64_t)n) { assert(ahead.read(&buf[0], 10) == 0); continue; }
            size_t want = 1 + rng() % 200000;
            int64_t got = ahead.read(&buf[0], want);
            size_t expect = std::min<size_t>(want, n - (size_t)at);
            assert(got >= 0 && (size_t)got <= expect);
            if (expect) assert(got > 0);
            assert(!std::memcmp(buf.data(), data.data() + at, (size_t)got));
            usleep(rng() % 3 ? 0 : 3000); // sometimes let the helper run ahead
        }
        assert(ahead.seek(0, SEEK_END) == (int64_t)n && ahead.read(&buf[0], 5) == 0);
        assert(ahead.seek(-1, SEEK_SET) == -1);
    }
    // Closing while the helper is busy, and reopening.
    for (int i = 0; i < 20; ++i) { ReadAhead ahead; assert(ahead.open(open_reader(path))); char b[10]; ahead.read(b, 10); }
    // Stats survive close().
    { ReadAhead ahead; assert(ahead.open(open_reader(path))); char b[1000]; ahead.read(b, 1000); usleep(50000); ahead.close(); assert(ahead.stats().bytes > 0); }

    std::string cmd = "rm -rf " + dir; (void)!system(cmd.c_str());
    std::cout << "Vault read-ahead: sequential, random seeks, close while busy PASS\n";
}
