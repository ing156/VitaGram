#pragma once
#include <cstdint>
#include <set>
#include <utility>

class SpoilerVisibility {
public:
    struct Context {
        int64_t chat = 0;
        int32_t topic = 0;
        bool topics = false, comments = false;
        bool operator==(const Context& other) const {
            return chat == other.chat && topic == other.topic && topics == other.topics && comments == other.comments;
        }
    };
    bool revealed(int64_t chat, int64_t message) const { return revealed_.count({chat, message}) != 0; }
    void reveal(int64_t chat, int64_t message) { if (chat && message) revealed_.insert({chat, message}); }
    // Report invalidation so the UI can discard sharp textures before the next frame.
    bool observe(Context next, bool hide_on_chat_exit) {
        if (next == context_) return false;
        const bool changed = hide_on_chat_exit && context_.chat && !context_.topics && hideChat(context_.chat);
        context_ = next;
        return changed;
    }
    bool appLeft(bool hide_on_app_exit) { return hide_on_app_exit && reset(); }
    bool reset() { const bool changed = !revealed_.empty(); revealed_.clear(); return changed; }
private:
    bool hideChat(int64_t chat) {
        auto first = revealed_.lower_bound({chat, INT64_MIN});
        auto last = revealed_.upper_bound({chat, INT64_MAX});
        const bool changed = first != last;
        revealed_.erase(first, last);
        return changed;
    }
    Context context_;
    std::set<std::pair<int64_t, int64_t>> revealed_;
};
