#pragma once
#include <map>
#include <string>
#include <cstdint>

// UI-owned cooldowns: failed textures must not become permanent cache entries,
// but a damaged file must not be decoded on every frame either.
class ImageRetry {
    struct Entry { unsigned failures = 0; int64_t next = 0; };
    std::map<std::string, Entry> entries_;
public:
    bool ready(const std::string& path, int64_t now) const {
        auto it = entries_.find(path);
        return it == entries_.end() || now >= it->second.next;
    }
    unsigned fail(const std::string& path, int64_t now) {
        if (!entries_.count(path) && entries_.size() >= 256) entries_.erase(entries_.begin());
        auto& entry = entries_[path];
        if (entry.failures < 4) ++entry.failures;
        const int64_t delays[] = {1000000, 3000000, 10000000, 30000000};
        entry.next = now + delays[entry.failures - 1];
        return entry.failures;
    }
    void forget(const std::string& path) { entries_.erase(path); }
    void clear() { entries_.clear(); }
};
