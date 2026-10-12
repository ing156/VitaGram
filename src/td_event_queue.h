#pragma once
#include <list>
#include <unordered_map>
#include <cstdint>
#include <iterator>
#include <jansson.h>
#include <cstring>

// Queue budget (wire bytes and events, raw + parsed). Above it, updates that only carry a current
// value the next update replaces (online status, "typing...", online member counts, emoji click
// effects) are dropped before parsing. Nothing else is: losing other updates would desynchronise
// the client, and receive() must keep running because it also drives TDLib's sockets on the Vita.
constexpr size_t kTdQueueHighWaterBytes = 8u << 20, kTdQueueHighWaterEvents = 4000;

// Hard limit for everything else. In the single-threaded Vita port TDLib only runs inside
// receive(): while it is not called, TDLib reads no sockets and produces no updates, so pausing it
// is real back-pressure (the server waits; a long pause may cost a reconnect, never data). The
// pause starts above the hard limit and ends once parsing and the UI have brought the queues back
// under the high-water mark. Wire bytes are counted; parsed JSON takes more memory than that.
constexpr size_t kTdQueueHardBytes = 16u << 20, kTdQueueHardEvents = 8000;
inline bool td_queue_paused(bool paused, size_t bytes, size_t events) {
    if (paused) return bytes > kTdQueueHighWaterBytes || events > kTdQueueHighWaterEvents;
    return bytes > kTdQueueHardBytes || events > kTdQueueHardEvents;
}

// `raw` is TDLib's JSON, which always starts with its "@type". The closing quote is part of the
// match, so "updateChatAction" does not also catch the persistent "updateChatActionBar".
inline bool td_event_is_ephemeral(const char* raw) {
    for (const char* prefix : {"{\"@type\":\"updateUserStatus\"", "{\"@type\":\"updateChatAction\"",
                               "{\"@type\":\"updateChatOnlineMemberCount\"", "{\"@type\":\"updateAnimatedEmojiMessageClicked\""})
        if (!std::strncmp(raw, prefix, std::strlen(prefix))) return true;
    return false;
}

// Original JSON payload bytes, not the size of its parsed DOM allocations.
inline bool td_event_changes_chat_list(json_t* root) {
    const char* type = json_string_value(json_object_get(root, "@type"));
    if (!type) return false;
    for (const char* relevant : {"chat", "updateNewChat", "updateChatTitle", "updateChatPhoto",
         "updateChatPosition", "updateChatLastMessage", "updateChatDraftMessage", "updateChatFolders",
         "updateNewMessage", "updateMessageSendSucceeded", "updateMessageSendFailed", "updateAuthorizationState"})
        if (!std::strcmp(type, relevant)) return true;
    return !std::strncmp(type, "authorizationState", 18);
}

inline size_t td_event_wire_bytes(json_t* root) {
    json_int_t bytes = json_integer_value(json_object_get(root, "@wire_bytes"));
    return bytes > 0 ? static_cast<size_t>(bytes) : 0;
}

inline bool urgent_td_event(json_t* root) {
    const char* type = json_string_value(json_object_get(root, "@type"));
    if (!type) return false;
    if (!std::strcmp(type, "updateAuthorizationState") || !std::strcmp(type, "updateConnectionState") ||
        !std::strcmp(type, "updateMessageSendSucceeded") || !std::strcmp(type, "updateMessageSendFailed") ||
        !std::strcmp(type, "error")) return true;
    auto* extra = json_object_get(root, "@extra");
    const char* kind = json_string_value(json_object_get(extra, "kind"));
    return (kind && (!std::strcmp(kind, "media_upload") || !std::strcmp(kind, "proxy_check") ||
                    !std::strcmp(kind, "proxy_config") || !std::strncmp(kind, "gif_", 4))) ||
        (json_is_string(extra) && !std::strcmp(json_string_value(extra), "send_message"));
}

// Prepared once on Parser, before taking the shared queue mutex.
enum class TdResponseKind { None, History, Reaction, Forum, Post, Sticker, Gif };
struct TdEventMetadata {
    int64_t chat=0, generation=0;
    int32_t file=0;
    size_t bytes=0;
    TdResponseKind response=TdResponseKind::None;
    int64_t received=0, parsed=0, requested=0;
    bool first_history=false, changes_chats=false;
    bool urgent=false, history=false;
};
inline TdEventMetadata describe_td_event(json_t* root) {
    TdEventMetadata m;m.urgent=urgent_td_event(root);m.bytes=td_event_wire_bytes(root);
    m.received=json_integer_value(json_object_get(root,"@received_at"));
    m.parsed=json_integer_value(json_object_get(root,"@parsed_at"));
    m.changes_chats=td_event_changes_chat_list(root);
    const char* type=json_string_value(json_object_get(root,"@type"));
    auto* extra=json_object_get(root,"@extra");
    const char* kind=json_string_value(json_object_get(extra,"kind"));
    if (kind && !std::strncmp(kind, "gif_", 4)) {
        m.response = TdResponseKind::Gif;
        m.chat = json_integer_value(json_object_get(extra, "chat"));
        m.generation = json_integer_value(json_object_get(extra, "generation"));
    } else if (kind && !std::strcmp(kind,"history")) {
        m.response=TdResponseKind::History;
        m.first_history=type && !std::strcmp(type,"messages") && json_is_true(json_object_get(extra,"first"));
        m.requested=json_integer_value(json_object_get(extra,"requested_at"));
        m.history=true;m.chat=json_integer_value(json_object_get(extra,"chat"));
        m.generation=json_integer_value(json_object_get(extra,"generation"));
    } else if (kind && (!std::strncmp(kind,"reaction_",9) || !std::strncmp(kind,"forum_",6) ||
                       !std::strncmp(kind,"post_",5) || !std::strcmp(kind,"sticker_items"))) {
        m.chat=json_integer_value(json_object_get(extra,"chat"));
        m.response=!std::strncmp(kind,"reaction_",9) ? TdResponseKind::Reaction :
            !std::strncmp(kind,"forum_",6) ? TdResponseKind::Forum :
            !std::strncmp(kind,"post_",5) ? TdResponseKind::Post : TdResponseKind::Sticker;
    } else {
        m.chat=json_integer_value(json_object_get(root,"chat_id"));
        if (!m.chat) m.chat=json_integer_value(json_object_get(json_object_get(root,"message"),"chat_id"));
        if (!m.chat && type && !std::strcmp(type,"chat")) m.chat=json_integer_value(json_object_get(root,"id"));
    }
    if (type && !std::strcmp(type,"updateFile") && !extra)
        m.file=static_cast<int32_t>(json_integer_value(json_object_get(json_object_get(root,"file"),"id")));
    return m;
}

// Non-owning container: take transfers the reference, clear requires callers to
// release queued roots first. Only replacing an unsolicited file frees a root.
// Nodes and bucket iterators stay stable across inserts; selection never reads JSON.
class TdEventQueue {
    struct Node;
    using Nodes=std::list<Node>;
    using Bucket=std::list<Node*>;
    struct Node {
        void* root;
        TdEventMetadata meta;
        uint64_t serial;
        Nodes::iterator position;
        Bucket::iterator bucket_position;
    };
    Nodes normal_;
    struct Urgent { void* root; TdEventMetadata meta; };
    std::list<Urgent> urgent_;
    std::unordered_map<int64_t,Bucket> chats_;
    std::unordered_map<int64_t,std::unordered_map<int64_t,Bucket>> histories_;
    std::unordered_map<int32_t,Node*> files_;
    uint64_t serial_=0;
    void* remove(Node* n,TdEventMetadata* metadata=nullptr) {
        if (metadata) *metadata=n->meta;
        if (n->meta.chat) {
            if (n->meta.history) {
                auto chat=histories_.find(n->meta.chat);
                auto generation=chat->second.find(n->meta.generation);
                generation->second.erase(n->bucket_position);
                if (generation->second.empty()) chat->second.erase(generation);
                if (chat->second.empty()) histories_.erase(chat);
            } else {
                auto chat=chats_.find(n->meta.chat);chat->second.erase(n->bucket_position);
                if (chat->second.empty()) chats_.erase(chat);
            }
        }
        if (n->meta.file) files_.erase(n->meta.file);
        void* root=n->root;normal_.erase(n->position);return root;
    }
    Node* foreground(int64_t chat,int64_t generation) {
        Node* result=nullptr;
        auto direct=chats_.find(chat);
        if (direct!=chats_.end()) result=direct->second.front();
        auto histories=histories_.find(chat);
        if (histories!=histories_.end()) {
            auto active=histories->second.find(generation);
            if (active!=histories->second.end()) {
                auto* candidate=active->second.front();
                if (!result || candidate->serial<result->serial) result=candidate;
            }
        }
        return result;
    }
public:
    TdEventQueue()=default;
    TdEventQueue(const TdEventQueue&)=delete;
    TdEventQueue& operator=(const TdEventQueue&)=delete;
    size_t size() const { return normal_.size()+urgent_.size(); }
    bool empty() const { return normal_.empty() && urgent_.empty(); }
    size_t enqueue(json_t* root,const TdEventMetadata& m) {
        size_t removed=0;
        if (m.file) {
            auto old=files_.find(m.file);
            if (old!=files_.end()) {
                removed=old->second->meta.bytes;
                json_decref(static_cast<json_t*>(remove(old->second)));
            }
        }
        if (m.urgent) { urgent_.push_back({root,m});return removed; }
        normal_.push_back({root,m,++serial_,{}, {}});
        auto it=std::prev(normal_.end());auto* n=&*it;n->position=it;
        if (m.chat) {
            auto& bucket=m.history ? histories_[m.chat][m.generation] : chats_[m.chat];
            bucket.push_back(n);n->bucket_position=std::prev(bucket.end());
        }
        if (m.file) files_[m.file]=n;
        return removed;
    }
    void push_back(void* root) { enqueue(static_cast<json_t*>(root),describe_td_event(static_cast<json_t*>(root))); }
    void* take(bool interactive,bool background_turn,int64_t chat=0,int64_t generation=0,TdEventMetadata* metadata=nullptr) {
        if (empty()) return nullptr;
        if (interactive) {
            if (urgent_.empty()) return nullptr;
            if (metadata) *metadata=urgent_.front().meta;
            void* root=urgent_.front().root;urgent_.pop_front();return root;
        }
        if (background_turn && !normal_.empty()) return remove(&normal_.front(),metadata);
        if (!urgent_.empty()) { if (metadata) *metadata=urgent_.front().meta;
            void* root=urgent_.front().root;urgent_.pop_front();return root; }
        if (chat) if (auto* n=foreground(chat,generation)) return remove(n,metadata);
        return normal_.empty() ? nullptr : remove(&normal_.front(),metadata);
    }
    void* front() const { return urgent_.empty() ? normal_.front().root : urgent_.front().root; }
    void pop_front() { if (!urgent_.empty()) urgent_.pop_front();else remove(&normal_.front()); }
    void clear() { normal_.clear();urgent_.clear();chats_.clear();histories_.clear();files_.clear(); }
    class const_iterator {
        const TdEventQueue* owner_;
        bool urgent;
        std::list<Urgent>::const_iterator u;
        Nodes::const_iterator n;
    public:
        const_iterator(const TdEventQueue* owner,bool end):owner_(owner),urgent(!end && !owner->urgent_.empty()),
            u(owner->urgent_.begin()),n(end ? owner->normal_.end() : owner->normal_.begin()) {}
        void* operator*() const { return urgent ? u->root : n->root; }
        const_iterator& operator++() { if (urgent) { if (++u==owner_->urgent_.end()) urgent=false; } else ++n;return *this; }
        bool operator!=(const const_iterator& other) const { return urgent!=other.urgent || (urgent ? u!=other.u : n!=other.n); }
    };
    const_iterator begin() const { return const_iterator(this,false); }
    const_iterator end() const { return const_iterator(this,true); }
};
inline size_t enqueue_td_event(TdEventQueue& queue,json_t* root) { return queue.enqueue(root,describe_td_event(root)); }
inline void* take_td_event(TdEventQueue& queue,bool interactive,bool background_turn,int64_t chat=0,int64_t generation=0,TdEventMetadata* metadata=nullptr) {
    return queue.take(interactive,background_turn,chat,generation,metadata);
}

// This unsolicited update has no consumer in this client. Preserve the event
// marker and timing; release the unused set DOM on Parser, never on UI.
inline json_t* compact_unused_td_update(json_t* root) {
    const char* type=json_string_value(json_object_get(root,"@type"));
    if (!type || std::strcmp(type,"updateStickerSet") || json_object_get(root,"@extra")) return nullptr;
    auto* compact=json_object();
    for (const char* field:{"@type","@received_at","@wire_bytes"})
        json_object_set(compact,field,json_object_get(root,field));
    return compact;
}

// Raw reply check on the receiving thread, before any parsing: TDLib appends
// "@extra" last, so only the tail is inspected (key order inside is ours).
inline bool td_reply_is_foreground(const char* text) {
    if (!text) return false;
    const size_t length=std::strlen(text);
    const char* tail=text+(length>400 ? length-400 : 0);
    const char* extra=std::strstr(tail,"\"@extra\":{");
    if (!extra) return false;
    const char* kind=std::strstr(extra,"\"kind\":\"");
    if (!kind) return false;
    kind+=8;
    return !std::strncmp(kind,"history\"",8) || !std::strncmp(kind,"reaction_",9) || !std::strncmp(kind,"forum_",6) || !std::strncmp(kind,"gif_",4);
}
