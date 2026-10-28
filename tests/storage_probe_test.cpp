#include "storage_probe.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#ifdef __vita__
#include <fcntl.h>
#include <unistd.h>
int sceIoOpen(const char* path, int flags, int mode) {
    int host_flags;
    if (flags == (SCE_O_RDWR | SCE_O_CREAT | SCE_O_EXCL)) {
        assert(mode == 0666); // Match newlib's known-working native creation mode.
        host_flags = O_RDWR | O_CREAT | O_EXCL;
    } else { assert(flags == SCE_O_RDONLY && mode == 0); host_flags = O_RDONLY; }
    return ::open(path, host_flags, mode);
}
int sceIoClose(int fd) { return ::close(fd); }
int sceIoPread(int fd, void* data, unsigned size, int64_t offset) { return ::pread(fd, data, size, offset); }
int sceIoPwrite(int fd, const void* data, unsigned size, int64_t offset) { return ::pwrite(fd, data, size, offset); }
int sceIoRemove(const char* path) { return ::unlink(path); }
int sceIoChstatByFd(int fd, const SceIoStat* stat, unsigned int cbit) { assert(cbit == 0x0004); return ::ftruncate(fd, stat->st_size); }
#endif
static bool proceed(void* value) { return *static_cast<bool*>(value); }
int main(int argc, char** argv) {
    assert(argc == 2);
    std::string directory = argv[1], path = directory + "/vitagram-storage-probe.tmp";
    bool allowed = true;
    FILE* log = std::tmpfile(); assert(log);
    storage_probe::run(directory, storage_probe::reserve, log, proceed, &allowed);
    assert(access(path.c_str(), F_OK) != 0);
    { std::ofstream file(path); file << "preserve"; }
    storage_probe::run(directory, storage_probe::reserve + storage_probe::file_bytes, log, proceed, &allowed);
    { std::ifstream file(path); std::string data; file >> data; assert(data == "preserve"); }
    assert(unlink(path.c_str()) == 0);
    allowed = false;
    storage_probe::run(directory, storage_probe::reserve + storage_probe::file_bytes, log, proceed, &allowed);
    assert(access(path.c_str(), F_OK) != 0);
    allowed = true;
    storage_probe::run(directory, storage_probe::reserve + storage_probe::file_bytes, log, proceed, &allowed);
    assert(access(path.c_str(), F_OK) != 0);
    std::atomic<int> budget{80};
    storage_probe::run(directory, storage_probe::reserve + storage_probe::file_bytes, log,
        [](void* p) { return static_cast<std::atomic<int>*>(p)->fetch_sub(1) > 0; }, &budget);
    assert(access(path.c_str(), F_OK) != 0); // Cancellation after temporary data has been written.
    storage_probe::extension(directory, storage_probe::reserve, log);
    storage_probe::extension(directory, storage_probe::reserve + storage_probe::extend_bytes, log);
    assert(access((directory + "/vitagram-storage-extend.tmp").c_str(), F_OK) != 0);
    std::rewind(log);
    std::string output; char line[512]; while (std::fgets(line, sizeof(line), log)) output += line;
    assert(output.find("result=skip_space") != std::string::npos);
    assert(output.find("result=open_failed") != std::string::npos);
    assert(output.find("result=cancelled") != std::string::npos);
    assert(output.find("result=incomplete") != std::string::npos);
    assert(output.find("result=ok") != std::string::npos);
    assert(output.find("extend result=skip_space") != std::string::npos);
    assert(output.find("extend mode=far_write resize_rc=0 resize_us=0 far_write_rc=65536") != std::string::npos);
    assert(output.find("extend mode=resize_then_write resize_rc=0") != std::string::npos);
    assert(output.find("concurrent_read_64k n=64") != std::string::npos);
    assert(output.find("concurrent_write_64k n=64") != std::string::npos);
    std::fclose(log);
    std::puts("Storage probe: space reserve, existing-file protection, cancellation, concurrent verified reads/writes and cleanup passed");
}
