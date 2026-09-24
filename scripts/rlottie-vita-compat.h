#pragma once
// Forced into rlottie translation units before upstream headers. Feature-test
// macros are supplied on the compiler command line, before any libc header.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

// Upstream falls back to Windows MAX_PATH when PATH_MAX isn't exposed.
// Keep its canonical-path validation intact and supply a bounded buffer size.
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
