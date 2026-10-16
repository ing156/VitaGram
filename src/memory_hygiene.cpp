#include "memory_hygiene.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#ifdef __vita__
#include <malloc.h>
#endif

namespace memory_hygiene {
namespace {
std::atomic<bool> g_enabled{false};
void* (*volatile g_memset)(void*, int, size_t) = std::memset;
}

void set_enabled(bool enabled) { g_enabled.store(enabled, std::memory_order_relaxed); }
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

size_t wipe_length(size_t usable, bool enabled) { return enabled && usable <= kMaxWipe ? usable : 0; }

void clear(void* data, size_t length) { if (data && length) g_memset(data, 0, length); }
}

#ifdef __vita__
extern "C" void __real_free(void* block);
extern "C" void __wrap_free(void* block) {
    if (block) memory_hygiene::clear(block, memory_hygiene::wipe_length(malloc_usable_size(block), memory_hygiene::enabled()));
    __real_free(block);
}
#endif
