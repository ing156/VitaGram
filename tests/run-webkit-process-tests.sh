#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
work=${VITAGRAM_WEBKIT_WORK:-$root/external/webkit-vita-work}
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-webkit-process.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
mkdir -p "$build_dir/wtf"
cat > "$build_dir/config.h" <<'EOF'
#pragma once
#include <cassert>
#define PLATFORM(x) PLATFORM_##x
#define PLATFORM_VITA 1
#define ASSERT(x) assert(x)
EOF
cat > "$build_dir/wtf/RunLoop.h" <<'EOF'
#pragma once
namespace WTF {
class RunLoop { public: static RunLoop& mainSingleton(); };
extern thread_local RunLoop* owner;
}
using WTF::RunLoop;
EOF
cat > "$build_dir/wtf/MainThread.h" <<'EOF'
#pragma once
inline bool isUIThread() { return true; }
EOF
cat > "$build_dir/ProcessIdentifier.h" <<'EOF'
#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
namespace WebCore {
class ProcessIdentifier {
    uint64_t value;
public:
    explicit ProcessIdentifier(uint64_t value) : value(value) { }
    uint64_t toUInt64() const { return value; }
    bool operator==(const ProcessIdentifier&) const = default;
};
namespace Process {
ProcessIdentifier generateIdentifier();
void setIdentifier(ProcessIdentifier);
ProcessIdentifier identifier();
}
}
EOF
cp "$work/source/Source/WebCore/platform/ProcessIdentifier.cpp" "$build_dir/ProcessIdentifier.cpp"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread -I "$build_dir" \
  "$root/tests/webkit_process_test.cpp" "$build_dir/ProcessIdentifier.cpp" -o "$build_dir/process"
"$build_dir/process"
# Exercise the real close-handler and callback-selection code with platform stubs.
python3 "$root/tests/webkit_shutdown_test.py" "$work/source" "$build_dir"
mkdir -p "$build_dir/psp2/kernel" "$build_dir/psp2/io"
cat > "$build_dir/psp2/kernel/threadmgr.h" <<'EOF'
#pragma once
#include <stdint.h>
typedef int SceUID;
typedef uint32_t SceSize;
typedef uint32_t SceUInt;
typedef int (*SceKernelThreadEntry)(SceSize, void*);
struct SceKernelThreadOptParam;
extern "C" SceUID sceKernelCreateThread(const char*, SceKernelThreadEntry, int, SceSize, SceUInt, int, const SceKernelThreadOptParam*);
extern "C" int sceKernelStartThread(SceUID, SceSize, void*);
extern "C" int sceKernelWaitThreadEnd(SceUID, int*, SceUInt*);
extern "C" int sceKernelDeleteThread(SceUID);
EOF
touch "$build_dir/psp2/io/fcntl.h"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread -DEGL_NO_PLATFORM_SPECIFIC_TYPES=1 \
  -I "$build_dir" -I "$root/src" -I "$root/external/webkit-vita/include/wpe-1.0" \
  -I "$root/external/webkit-vita/include" \
  "$root/tests/webkit_provider_test.cpp" "$root/src/webview/ProcessProviderVita.cpp" -o "$build_dir/provider"
"$build_dir/provider"
python3 "$root/tests/webapp_input_test.py" "$root" "$build_dir" "$work/source"
