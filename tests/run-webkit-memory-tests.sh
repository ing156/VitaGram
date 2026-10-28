#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
work=${VITAGRAM_WEBKIT_WORK:-$root/external/webkit-vita-work}
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-webkit-memory.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
# Compile the port's real descriptor wrapper; stub only unrelated WTF facilities.
mkdir -p "$build_dir/wtf"
cat > "$build_dir/wtf/Compiler.h" <<'EOF'
#pragma once
#include <cassert>
#define PLATFORM(x) 1
#define WARN_UNUSED_RETURN __attribute__((warn_unused_result))
#define RELEASE_ASSERT(x) assert(x)
EOF
cat > "$build_dir/wtf/Noncopyable.h" <<'EOF'
#pragma once
#define WTF_MAKE_NONCOPYABLE(T) T(const T&) = delete; T& operator=(const T&) = delete
EOF
cat > "$build_dir/wtf/StdLibExtras.h" <<'EOF'
#pragma once
#include <utility>
#define WTFMove(x) std::move(x)
EOF
cat > "$build_dir/wtf/UniStdExtras.h" <<'EOF'
#pragma once
#include <unistd.h>
namespace WTF {
inline int dupCloseOnExec(int fd) { return dup(fd); }
inline void closeWithRetry(int fd) { close(fd); }
}
EOF
cat > "$build_dir/config.h" <<'EOF'
#pragma once
EOF
cat > "$build_dir/wtf/Ref.h" <<'EOF'
#pragma once
EOF
cat > "$build_dir/wtf/RefPtr.h" <<'EOF'
#pragma once
#include <memory>
template<class T> using RefPtr = std::shared_ptr<T>;
template<class T> RefPtr<T> adoptRef(T* value) { return RefPtr<T>(value); }
EOF
# A small WebCore type shim lets the real SharedMemoryVita.cpp run on the host.
cat > "$build_dir/SharedMemory.h" <<'EOF'
#pragma once
#include <optional>
#include <span>
#include <wtf/RefPtr.h>
#include <wtf/unix/UnixFileDescriptor.h>
namespace WebCore {
class SharedMemoryHandle {
public:
    SharedMemoryHandle(UnixFileDescriptor&& handle, uint64_t size) : m_handle(std::move(handle)), m_size(size) {}
    SharedMemoryHandle(const SharedMemoryHandle&);
    SharedMemoryHandle(SharedMemoryHandle&&) = default;
    UnixFileDescriptor releaseHandle();
    size_t size() const { return m_size; }
private:
    friend class SharedMemory;
    UnixFileDescriptor m_handle;
    uint64_t m_size;
};
class SharedMemory {
public:
    using Handle = SharedMemoryHandle;
    enum class Protection : bool { ReadOnly, ReadWrite };
    static RefPtr<SharedMemory> allocate(size_t);
    static RefPtr<SharedMemory> map(Handle&&, Protection);
    static RefPtr<SharedMemory> wrapMap(void*, size_t, int);
    std::optional<Handle> createHandle(Protection);
    ~SharedMemory();
    size_t size() const { return m_size; }
    std::span<uint8_t> mutableSpan() { return {static_cast<uint8_t*>(m_data), m_size}; }
private:
    size_t m_size;
    void* m_data;
    UnixFileDescriptor m_fileDescriptor;
};
}
EOF
cp "$work/source/Source/WebCore/platform/SharedMemoryVita.cpp" "$build_dir/SharedMemoryVita.cpp"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread \
  -I "$build_dir" -I "$root/src" -I "$work/source/Source/WTF" \
  "$root/tests/webkit_memory_test.cpp" "$build_dir/SharedMemoryVita.cpp" \
  -Wl,--wrap=free -o "$build_dir/memory"
"$build_dir/memory"
