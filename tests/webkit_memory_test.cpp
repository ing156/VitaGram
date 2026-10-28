#include "webview/VitaSharedMemory.h"
#include <wtf/unix/UnixFileDescriptor.h>
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <vector>
#include <cstdio>
#include "SharedMemory.h"
extern "C" unsigned vitagram_webkit_ram_transport_version();
static void* wipeTarget = nullptr;
static size_t wipeSize = 0;
static bool wipeObserved = false;
extern "C" void __real_free(void*);
extern "C" void __wrap_free(void* data) {
    if (data && data == wipeTarget) {
        auto* bytes = static_cast<unsigned char*>(data);
        for (size_t i = 0; i < wipeSize; ++i) assert(bytes[i] == 0);
        wipeObserved = true;
        wipeTarget = nullptr;
    }
    __real_free(data);
}

int main() {
    assert(vitagram_webkit_ram_transport_version() == 1);
    using Buffer = Vita::SharedBuffer;
    using Channel = Vita::AttachmentChannel<UnixFileDescriptor>;
    assert(!Buffer::allocate(0));
    assert(!Buffer::allocate(Buffer::budget + 1));
    {
        auto memory = Buffer::allocate(Buffer::budget);
        assert(memory && !Buffer::allocate(1));
        auto* bytes = static_cast<unsigned char*>(memory->data);
        assert(bytes[0] == 0 && bytes[memory->size - 1] == 0);
    }
    std::vector<unsigned char> wipe(2 * 1024 * 1024, 0x55);
    Buffer::clear(wipe.data(), wipe.size());
    for (auto byte : wipe) assert(byte == 0);
    std::weak_ptr<Buffer> lifetime;
    auto sender = Channel::acquire(12);
    auto receiver = Channel::acquire(12);
    assert(sender == receiver);
    auto memory = Buffer::allocate(5 * 1024 * 1024);
    wipeTarget = memory->data;
    wipeSize = memory->size;
    lifetime = memory;
    UnixFileDescriptor source{memory};
    assert(source && source.value() == -1);
    auto ticket = sender->publish(source.duplicate());
    assert(ticket);
    // IPC can create a handle before filling its body; it must reference live RAM.
    std::memset(memory->data, 0x5a, memory->size);
    source = {}; memory.reset();
    assert(!lifetime.expired());
    std::thread consumer([&] {
        auto mapped = receiver->take(ticket);
        assert(mapped && mapped.sharedMemory()->size == 5 * 1024 * 1024);
        auto* data = static_cast<unsigned char*>(mapped.sharedMemory()->data);
        for (size_t i = 0; i < mapped.sharedMemory()->size; ++i) assert(data[i] == 0x5a);
        assert(!receiver->take(ticket));
        auto copy = mapped.duplicate();
        auto moved = std::move(copy);
        assert(!copy && moved);
        mapped = {};
        assert(!lifetime.expired());
    });
    consumer.join();
    assert(lifetime.expired() && wipeObserved);
    assert(!receiver->take(0) && !receiver->take(UINT64_MAX));
    assert(Channel::acquire(13)->take(ticket).value() == -1);
    // Failed sends and disconnects release unreceived buffers, including large ones.
    for (int i = 0; i < 1024; ++i) assert(sender->publish(UnixFileDescriptor{Buffer::allocate(1)}));
    assert(!sender->publish(UnixFileDescriptor{Buffer::allocate(1)}));
    receiver->close();
    assert(!sender->publish(UnixFileDescriptor{Buffer::allocate(1)}));
    assert(!receiver->take(ticket));
    auto all = Buffer::allocate(Buffer::budget);
    assert(all);
    all.reset();
    sender.reset(); receiver.reset();
    sender = Channel::acquire(12);
    assert(sender->publish(UnixFileDescriptor{Buffer::allocate(16)}));
    sender->close();
    // Ordinary descriptors still duplicate, move, release and close normally.
    int fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    {
        UnixFileDescriptor original{fd, UnixFileDescriptor::Adopt};
        auto copy = original.duplicate();
        assert(copy && copy.value() != fd && !copy.sharedMemory());
        int released = copy.release();
        assert(!copy && close(released) == 0);
    }
    assert(fcntl(fd, F_GETFD) == -1);
    {
        using Memory = WebCore::SharedMemory;
        auto original = Memory::allocate(5 * 1024 * 1024);
        assert(original);
        auto handle = original->createHandle(Memory::Protection::ReadOnly);
        assert(handle);
        original->mutableSpan()[0] = 0x42;
        original->mutableSpan().back() = 0x7e;
        auto mapped = Memory::map(std::move(*handle), Memory::Protection::ReadOnly);
        assert(mapped && mapped->size() == original->size());
        original.reset();
        assert(mapped->mutableSpan()[0] == 0x42 && mapped->mutableSpan().back() == 0x7e);
        auto secondHandle = mapped->createHandle(Memory::Protection::ReadOnly);
        assert(secondHandle);
        auto second = Memory::map(std::move(*secondHandle), Memory::Protection::ReadOnly);
        assert(second && second->mutableSpan().data() == mapped->mutableSpan().data());
        auto small = Buffer::allocate(4);
        assert(!Memory::map(Memory::Handle{UnixFileDescriptor{small}, 5}, Memory::Protection::ReadOnly));
        assert(!Memory::map(Memory::Handle{UnixFileDescriptor{small}, 0}, Memory::Protection::ReadOnly));
        assert(!Memory::map(Memory::Handle{UnixFileDescriptor{}, 4}, Memory::Protection::ReadOnly));
        unsigned char borrowed[] = {1, 2, 3};
        auto wrapped = Memory::wrapMap(borrowed, sizeof(borrowed), -1);
        assert(wrapped && wrapped->mutableSpan()[2] == 3);
        borrowed[2] = 4;
        assert(wrapped->mutableSpan()[2] == 3);
    }
    std::puts("WebKit RAM: budget, zeroing, shared handles, large IPC, tickets, disconnects and real descriptors passed");
}
