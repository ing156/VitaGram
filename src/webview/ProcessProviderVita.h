#pragma once

#include <wpe/vita.h>

// Register before constructing WKContext/WKView. WebKit's logical web and
// network components then run on dedicated Vita threads with explicit IPC.
bool register_vita_process_provider();
