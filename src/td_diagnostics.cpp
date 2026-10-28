#include <cstdio>
#include "td_diagnostics.h"

extern "C" uint64_t vitagram_td_enter(unsigned phase) { return td_diagnostics::enter(phase); }
extern "C" void vitagram_td_leave(uint64_t previous) { td_diagnostics::leave(previous); }
extern "C" void vitagram_td_path(const char* path) { td_diagnostics::path(path); }
extern "C" void vitagram_td_file(const char* path, int fd, int64_t offset, uint64_t bytes) {
    td_diagnostics::file(path, fd, offset, bytes);
}
