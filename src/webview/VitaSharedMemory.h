#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace Vita {
// Only the Vita port's logical processes share an address space.
class SharedBuffer {
public:
    SharedBuffer(const SharedBuffer&) = delete;
    SharedBuffer& operator=(const SharedBuffer&) = delete;
    static constexpr size_t budget = 32 * 1024 * 1024;
    static std::shared_ptr<SharedBuffer> allocate(size_t size) {
        if (!size || size > budget) return {};
        auto used = bytes.load();
        do {
            if (used > budget - size) return {};
        } while (!bytes.compare_exchange_weak(used, used + size));
        void* data = std::calloc(1, size);
        if (!data) { bytes -= size; return {}; }
        return std::shared_ptr<SharedBuffer>(new SharedBuffer(data, size));
    }
    ~SharedBuffer() {
        clear(data, size);
        std::free(data);
        bytes -= size;
    }
    static void clear(void* data, size_t size) {
        auto* p = static_cast<volatile unsigned char*>(data);
        while (size--) *p++ = 0;
    }
    void* const data;
    const size_t size;
private:
    SharedBuffer(void* data, size_t size) : data(data), size(size) {}
    inline static std::atomic<size_t> bytes{0};
};

// Tickets retain attachments until receipt; closing either endpoint drops pending tickets.
template<class Attachment> class AttachmentChannel {
public:
    static std::shared_ptr<AttachmentChannel> acquire(uint64_t key) {
        static std::mutex registryLock;
        static std::map<uint64_t, std::weak_ptr<AttachmentChannel>> registry;
        std::lock_guard guard(registryLock);
        for (auto it = registry.begin(); it != registry.end(); ) {
            if (it->second.expired()) it = registry.erase(it); else ++it;
        }
        auto channel = registry[key].lock();
        if (!channel) { channel = std::make_shared<AttachmentChannel>(); registry[key] = channel; }
        return channel;
    }
    uint64_t publish(Attachment&& attachment) {
        std::lock_guard guard(lock);
        if (closed || !attachment || pending.size() >= 1024 || next == std::numeric_limits<uint64_t>::max()) return 0;
        auto ticket = ++next;
        pending.emplace(ticket, std::move(attachment));
        return ticket;
    }
    Attachment take(uint64_t ticket) {
        // Destroy the node outside the lock, including on malformed/duplicate tickets.
        typename std::map<uint64_t, Attachment>::node_type node;
        { std::lock_guard guard(lock); node = pending.extract(ticket); }
        return node.empty() ? Attachment{} : std::move(node.mapped());
    }
    void close() {
        std::map<uint64_t, Attachment> abandoned;
        { std::lock_guard guard(lock); closed = true; abandoned.swap(pending); }
    }
private:
    std::mutex lock;
    std::map<uint64_t, Attachment> pending;
    uint64_t next = 0;
    bool closed = false;
};
}
