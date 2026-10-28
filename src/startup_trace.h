#pragma once
#include "diag_log.h"

namespace startup_trace {
inline void write(const char* stage, bool reset = false) {
    std::fprintf(stderr, "Startup: %s\n", stage);
    if (FILE* file = diag_log::open(diag_log::Kind::Performance,
                                   "ux0:data/vitagram/startup.log", reset ? "wb" : "ab")) {
        std::fprintf(file, "%s\n", stage);
        std::fclose(file); // Preserve the last entered stage even if startup never returns.
    }
}
}
