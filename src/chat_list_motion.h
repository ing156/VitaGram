#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Rows keep their identity when the chat list is replaced (startup index -> Telegram,
// a chat moving to the top): a chat that changed place slides there from its previous
// row instead of the whole list being redrawn at once. Rows that are new or stayed put
// are drawn in place.
class ChatListMotion {
public:
    static constexpr int64_t kDurationUs = 180000;

    // Once per drawn frame. A different context (folder) or a list that was not drawn
    // recently starts without motion.
    void update(const std::vector<int64_t>& ids, int row_height, int context, int64_t now) {
        const bool continuous = context == context_ && now >= drawn_at_ && now - drawn_at_ < 200000;
        drawn_at_ = now;
        if (continuous && ids == ids_) return;
        std::vector<float> offsets(ids.size(), 0.f);
        if (continuous && !ids_.empty()) {
            std::unordered_map<int64_t, size_t> previous;
            previous.reserve(ids_.size());
            for (size_t i = 0; i < ids_.size(); ++i) previous.emplace(ids_[i], i);
            const float limit = 544.f;
            for (size_t i = 0; i < ids.size(); ++i) {
                auto found = previous.find(ids[i]);
                if (found == previous.end()) continue;
                const float moved = ((float)found->second - (float)i) * (float)row_height + offset(found->second, now);
                offsets[i] = std::clamp(moved, -limit, limit);
            }
        }
        ids_ = ids; offsets_ = std::move(offsets); started_ = now; context_ = context;
    }

    // Pixels to add to the row's resting y.
    float offset(size_t index, int64_t now) const {
        if (index >= offsets_.size() || !offsets_[index] || now >= started_ + kDurationUs) return 0.f;
        const float left = 1.f - (float)(now - started_) / (float)kDurationUs; // ease-out cubic
        return offsets_[index] * left * left * left;
    }

private:
    std::vector<int64_t> ids_;
    std::vector<float> offsets_;
    int64_t started_ = 0, drawn_at_ = 0;
    int context_ = 0;
};
