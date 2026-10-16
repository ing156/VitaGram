#pragma once
#include <cstddef>

// Zeroes heap blocks as they are freed while the data vault is on.
//
// The Vita has no swap, so RAM is never paged to the card - but freed heap blocks keep their
// contents until reused: old TDLib responses (messages, names, phone numbers), JSON requests carrying
// the database key or the two-step password, decrypted file chunks. Everything that ever passed
// through the heap would otherwise stay readable to a memory scanner (an injected plugin, a kernel
// debugger) and in crash dumps, long after the screen showed it. With wiping, only data still in use
// remains in RAM.
//
// The Vita build links with -Wl,--wrap=free, so every free() in the executable - C code, jansson,
// SQLite inside TDLib, and C++ operator delete in the static libstdc++ - goes through this. Blocks
// larger than kMaxWipe (decoded images, video and download buffers) are released unwiped: clearing
// megabytes on every free would cost frame time, and those buffers are recycled quickly.
// Not covered: blocks realloc() moves internally, and memory that is not from malloc (GPU memblocks).
namespace memory_hygiene {
constexpr size_t kMaxWipe = 1u << 20;

void set_enabled(bool enabled);
bool enabled();
// Bytes to clear of a block with `usable` bytes: all of it when enabled and at most kMaxWipe, else 0.
size_t wipe_length(size_t usable, bool enabled);
// memset the compiler cannot drop as a dead store.
void clear(void* data, size_t length);
}
