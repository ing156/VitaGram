#include "td/utils/port/FileFd.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
static int opened = 0, closed = 0, mode = 0;
static std::string trace_path;
static int trace_fd = -1;
static int64_t trace_offset = -1;
static uint64_t trace_bytes = 0;
extern "C" void vitagram_td_file(const char* path, int fd, int64_t offset, uint64_t bytes) {
    trace_path = path; trace_fd = fd; trace_offset = offset; trace_bytes = bytes;
}
int sceIoOpen(const char* path, int flags, int) {
    if (mode == 4) return static_cast<int>(0x80010002u);
    const int fd = ::open(path, flags); if (fd >= 0) ++opened; return fd;
}
int sceIoClose(int fd) { ++closed; return ::close(fd); }
int sceIoPwrite(int fd, const void* data, unsigned size, int64_t offset) {
    if (mode == 1) { mode = 0; return static_cast<int>(0x80010004u); }
    if (mode == 2) return static_cast<int>(0x8001001cu);
    return ::pwrite(fd, data, mode == 3 ? std::min(size, 2u) : size, offset);
}
int sceIoPread(int fd, void* data, unsigned size, int64_t offset) {
    if (mode == 1) { mode = 0; return static_cast<int>(0x80010004u); }
    if (mode == 2) return static_cast<int>(0x80010005u);
    return ::pread(fd, data, size, offset);
}
#include <sys/stat.h>
#include <psp2/io/stat.h>
static int resized = 0;
int sceIoGetstatByFd(int fd, SceIoStat* out) { struct stat s{}; const int rc = fstat(fd, &s); out->st_size = s.st_size; return rc; }
int sceIoChstatByFd(int fd, const SceIoStat* in, unsigned int bits) { assert(bits == 0x0004); ++resized; return ftruncate(fd, in->st_size); }
int main() {
    char path[] = "/tmp/vitagram-positioned-XXXXXX";
    int temporary = mkstemp(path); assert(temporary >= 0); ::close(temporary);
    auto file = td::FileFd::open(td::CSlice(static_cast<const char*>(path)), td::FileFd::Read | td::FileFd::Write).move_as_ok();
    mode = 4; assert(file.enable_native_positioned_io(td::CSlice(static_cast<const char*>(path)), true).is_error());
    mode = 0; file.enable_native_positioned_io(td::CSlice(static_cast<const char*>(path)), true).ensure();
    file.enable_native_positioned_io(td::CSlice(static_cast<const char*>(path)), true).ensure(); assert(opened == 1);
    file.seek(7).ensure();
    mode = 1; assert(file.pwrite("abcd", 1024).move_as_ok() == 4);
    assert(resized == 1); // A gap past EOF is sized before the write.
    assert(trace_path == path && trace_fd == file.get_native_fd().fd());
    assert(trace_offset == 1024 && trace_bytes == 4);
    assert(lseek(file.get_native_fd().fd(), 0, SEEK_CUR) == 7);
    assert(file.pwrite("ef", 1028).move_as_ok() == 2 && resized == 1); // Appends never resize.
    char data[4]{}; mode = 1;
    assert(file.pread(td::MutableSlice(data, 4), 1024).move_as_ok() == 4);
    assert(td::Slice(data, 4) == "abcd");
    mode = 2;
    assert(file.pwrite("fail", 0).is_error());
    assert(file.pread(td::MutableSlice(data, 4), 0).is_error());
    mode = 3; assert(file.pwrite("short", 0).move_as_ok() == 2);
    mode = 0; assert(file.pwrite("x", -1).is_error());
    assert(file.pread(td::MutableSlice(data, 4), -1).is_error());
    std::string renamed = std::string(path) + ".renamed";
    assert(!rename(path, renamed.c_str()));
    auto moved = std::move(file); assert(file.empty());
    assert(moved.pread(td::MutableSlice(data, 4), 1024).move_as_ok() == 4);
    assert(trace_path == path && trace_fd == moved.get_native_fd().fd());
    assert(trace_offset == 1024 && trace_bytes == 4);
    moved.close(); assert(closed == opened);
    auto read = td::FileFd::open(renamed, td::FileFd::Read).move_as_ok();
    read.enable_native_positioned_io(renamed, false).ensure();
    assert(read.pwrite("x", 0).is_error());
    auto fd = read.move_as_native_fd(); assert(closed == opened);
    assert(::pread(fd.fd(), data, 4, 1024) == 4);
    fd.close(); unlink(renamed.c_str());
    puts("TDLib positioned I/O: offsets, EINTR, short writes, errors, rename, move and close passed");
}
