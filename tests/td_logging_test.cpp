#include "td/utils/TsCerr.h"
#include "td/utils/logging.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

static int mode = 0, calls = 0;
static std::string delivered;
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size) {
    if (fd != 2) return __real_write(fd, data, size);
    ++calls;
    if (mode == 1) { errno = ENOSPC; return -1; }
    // Return real progress after three attempts so an old implementation fails
    // the assertion instead of hanging the regression test forever.
    if (mode == 2 && calls <= 3) return 0;
    const auto count = mode == 3 ? std::min<size_t>(2, size) : size;
    delivered.append(static_cast<const char*>(data), count);
    return count;
}
int main() {
    for (int failure : {1, 2}) {
        mode = failure; calls = 0;
        { td::TsCerr sink; sink << "message"; }
        assert(calls == 1);
        // The failed output must also release the global stderr lock.
        mode = 0; calls = 0; delivered.clear();
        { td::TsCerr sink; sink << "next"; }
        assert(calls == 1 && delivered == "next");
    }
    mode = 3; calls = 0; delivered.clear();
    { td::TsCerr sink; sink << "partial"; }
    assert(calls == 4 && delivered == "partial");
    mode = 0; calls = 0;
    int formatted = 0;
    td::set_verbosity_level(5);
    LOG(DEBUG) << ++formatted;
    assert(formatted == 1 && calls > 0);
    calls = 0; td::set_verbosity_level(1);
    LOG(DEBUG) << ++formatted;
    LOG(INFO) << ++formatted;
    assert(formatted == 1 && calls == 0);
    LOG(ERROR) << "error still visible";
    assert(calls > 0);
    std::puts("TDLib logging: disabled formatting, failed/zero writes, partial writes and lock release passed");
}
