#pragma once
#include <cstdint>
#include <string>

// A completed event is shown once, then yields to the current activity.
class TransientStatus {
public:
    void clear() { source_.clear(); until_ = 0; }
    std::string update(const std::string& source, int64_t now) {
        if (source != source_) {
            source_ = source;
            until_ = now + 8000000;
        }
        return now < until_ ? source_ : "";
    }
private:
    std::string source_;
    int64_t until_ = 0;
};
