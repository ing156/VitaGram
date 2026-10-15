#include "diag_log.h"
#include "td_dispatch_policy.h"
#include "i18n.h"
#include "td_unused_update.h"
#include "td_sticker_sets.h"
#include "sticker_preview.h"
#include "account_cleanup.h"
#include "telegram_client.h"
#include "proxy_link.h"
#include "video_note_message.h"
#include "video_quality.h"
#include "cpu_scheduler.h"
#include "td_event_queue.h"
#include "config.h"
#include "performance_log.h"
#include "file_download.h"
#include "secure_store.h"
#include "vault.h"
#include "rich_message.h"
#include "gif_diagnostics.h"
#include "user_identity.h"
#include "message_delete_policy.h"

#include <td/telegram/td_json_client.h>
#include <jansson.h>
#include <psp2/io/stat.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <utility>
#include <cctype>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/rtc.h>
#include <ctime>
#include "priority_ceiling.h"

// The UI waits on the event queue: background holders run at the UI priority meanwhile.
static void lock_queue(pthread_mutex_t* mutex) { priority_ceiling::enter(); performance::lock(mutex, performance::Metric::QueueLock); }
static void unlock_queue(pthread_mutex_t* mutex) { pthread_mutex_unlock(mutex); priority_ceiling::leave(); }

static int64_t json_i53(json_t* o, const char* key) {
    json_t* v = json_object_get(o, key);
    if (!v) return 0;
    if (json_is_integer(v)) return (int64_t)json_integer_value(v);
    if (json_is_string(v)) return std::strtoll(json_string_value(v), nullptr, 10);
    return 0;
}

static const char* json_str(json_t* o, const char* key, const char* def = "") {
    json_t* v = json_object_get(o, key);
    return json_is_string(v) ? json_string_value(v) : def;
}

// Freeing a parsed reply costs about as much as parsing it on the Vita. It stays
// inline: handing roots to another thread (0.33.49) made Parser and UI wait on
// the shared malloc lock and slowed the startup flood instead.
static void release_td_json(json_t* root,uint64_t wire_bytes=0) {
    performance::Scope metric(performance::Metric::JsonRelease);
    performance::SlowScope slow(performance::SlowPhase::Release,json_str(root,"@type"),wire_bytes ? wire_bytes : td_event_wire_bytes(root));
    json_decref(root);
}

// UI-thread only. Big replies are freed a few milliseconds per frame instead of
// in one 60-90 ms stall. The same thread frees them, so no malloc lock is shared
// with Parser (the 0.33.49 cross-thread hand-off did that and slowed startup).
struct RetiredJson { json_t* node; int depth; };
static std::vector<RetiredJson> retired_json;
static void retire_td_json(json_t* root,uint64_t wire_bytes=0) {
    if (!wire_bytes) wire_bytes=td_event_wire_bytes(root);
    if (wire_bytes<16*1024) { release_td_json(root,wire_bytes); return; }
    retired_json.push_back({root,0});
}
static void drain_retired_json(int64_t budget_us) {
    if (retired_json.empty()) return;
    performance::Scope metric(performance::Metric::JsonRelease);
    const int64_t started=sceKernelGetSystemTimeWide();
    while (!retired_json.empty() && sceKernelGetSystemTimeWide()-started<budget_us) {
        const auto item=retired_json.back(); retired_json.pop_back();
        json_t* node=item.node;
        const size_t children=json_is_array(node) ? json_array_size(node) : json_is_object(node) ? json_object_size(node) : 0;
        if (item.depth<6 && children>1) {
            // Detach children first; freeing the shell then leaves them alive.
            if (json_is_array(node)) { size_t i; json_t* v; json_array_foreach(node,i,v) { json_incref(v); retired_json.push_back({v,item.depth+1}); } }
            else { const char* k; json_t* v; json_object_foreach(node,k,v) { json_incref(v); retired_json.push_back({v,item.depth+1}); } }
        }
        json_decref(node);
    }
}

static int32_t chat_list_id(json_t* list) {
    const char* type = json_str(list, "@type");
    if (!std::strcmp(type, "chatListMain")) return 0;
    if (!std::strcmp(type, "chatListFolder")) return (int32_t)json_i53(list, "chat_folder_id");
    if (!std::strcmp(type, "chatListArchive")) return kArchiveFolderId;
    return -1;
}

// TDLib file IDs are session-local. Cached UI snapshots may display local paths,
// but must never use an old ID to download another file after a restart.
static void clear_cached_file_ids(json_t* root) {
    if (json_is_object(root)) {
        if (!std::strcmp(json_str(root, "@type"), "file")) { json_object_set_new(root, "id", json_integer(0)); return; }
        const char* key; json_t* value;
        json_object_foreach(root, key, value) clear_cached_file_ids(value);
    } else if (json_is_array(root)) {
        size_t i; json_t* value; json_array_foreach(root, i, value) clear_cached_file_ids(value);
    }
}

// Storage only: the local path of a completed small avatar whose file is still intact.
static std::string valid_avatar_path(json_t* file) {
    auto* local = json_object_get(file, "local");
    std::string path = json_is_true(json_object_get(local, "is_downloading_completed")) ? json_str(local, "path") : "";
    struct stat info{};
    const auto expected=json_i53(file,"size");
    if (path.empty() || stat(path.c_str(),&info) || !S_ISREG(info.st_mode) ||
        info.st_size<=0 || (expected>0 && info.st_size!=expected && info.st_size!=vault::sealed_size(expected))) path.clear();
    return path;
}
// The first rows are drawn with their avatars instead of placeholders that fill in one by one.
static void attach_startup_avatars(json_t* startup, bool (*proceed)(void*), void* context) {
    auto* avatars = json_object_get(startup, "avatars");
    const char* key; json_t* chat; size_t checked = 0;
    json_object_foreach(json_object_get(startup, "chats"), key, chat) {
        if (checked >= 40 || (proceed && !proceed(context))) break;
        auto* file = json_object_get(avatars, key);
        if (!file) continue;
        ++checked;
        const auto path = valid_avatar_path(file);
        if (!path.empty()) json_object_set_new(chat, "@avatar_path", json_string(path.c_str()));
    }
}
void* TelegramClient::prepareCache() {
    performance::Scope measurement(performance::Metric::CacheRestore);
    auto proceed_startup = [](void* opaque) -> bool { return !static_cast<TelegramClient*>(opaque)->stopping_; };
    if (auto* startup = ui_cache_.loadStartup()) { attach_startup_avatars(startup, proceed_startup, this); return startup; }
    // One-time migration from the monolithic cache. Subsequent starts read only
    // the small index before showing chats; full histories remain on Storage.
    ui_cache_.load(cacheLoadProceed, this); full_cache_loaded_ = true;
    auto* startup = ui_cache_.startupSnapshot();
    auto proceed = [](void* opaque) -> bool { return !static_cast<TelegramClient*>(opaque)->stopping_; };
    ui_cache_.saveStartup(startup, proceed, this);
    return startup;
}
void TelegramClient::restoreCache() {
    if (account_cleanup::pending() || logging_out_) return;

    // Synchronous compatibility path. Production uses restoreCacheAsync on Storage.
    auto* startup = prepareCache();
    if (startup) {
        applyRestoredCache(startup);
    }

    // Complete details before returning to synchronous callers.
    if (!full_cache_loaded_) {
        ui_cache_.load();
        full_cache_loaded_ = true;
    }
    media_cache_.load();
}
bool TelegramClient::restoreCacheAsync() {
    if (cache_worker_started_ || logging_out_ || account_cleanup::pending() || stopping_) return false;
    restore_on_cache_worker_ = true;
    startup_list_hold_ = startCacheWorker();
    return startup_list_hold_;
}
void TelegramClient::beginCacheApply(void* opaque) {
    if (applying_snapshot_) json_decref(static_cast<json_t*>(applying_snapshot_));
    applying_snapshot_ = opaque;
    cache_apply_stage_ = 0;
    // Once Telegram has confirmed a list in this session, the index is older than what is shown.
    cache_restore_chats_ = chats_last_updated_.empty();
}
void TelegramClient::applyRestoredCache(void* opaque) {
    // Synchronous compatibility path for offline tools/tests; production pump
    // calls one slice per frame, after protocol events.
    beginCacheApply(opaque);
    while (applying_snapshot_) applyCacheSlice();
}
void TelegramClient::applyCacheSlice() {
    auto* snapshot = static_cast<json_t*>(applying_snapshot_);
    if (!snapshot) return;
    performance::Scope measurement(performance::Metric::CacheApply);
    if (logging_out_ || !cache_restore_allowed_) {
        json_decref(snapshot); applying_snapshot_ = nullptr; return;
    }
    const auto started = sceKernelGetSystemTimeWide();
    bool changed = false;

    // The index holds only the top of each list: install all its chats in one step so the
    // list appears whole instead of growing row by row. Users and folders stay incremental.
    unsigned count = 0;
    const unsigned max_count = 4;

    for (; count < max_count || cache_apply_stage_ == 1; ++count) {
        if (cache_apply_stage_ == 0) {
            auto* folders = json_object_get(snapshot, "folders");
            if (folders && !live_folders_received_) parseFolders(folders);
            json_object_del(snapshot, "folders"); ++cache_apply_stage_;
        } else if (cache_apply_stage_ <= 2) {
            auto* collection = json_object_get(snapshot, cache_apply_stage_ == 1 ? "chats" : "users");
            auto* iter = json_object_iter(collection);
            if (!iter) { changed = changed || cache_apply_stage_ == 1; ++cache_apply_stage_; continue; }
            auto* value = json_object_iter_value(iter);
            const auto id = json_i53(value, "id");
            if (cache_apply_stage_ == 1 && cache_restore_chats_ && id) {
                // Live chats announced before loadChats keep their data; the startup rows
                // keep the previous order until Telegram confirms the visible window.
                auto known = std::find_if(known_chats_.begin(), known_chats_.end(), [&](const ChatItem& c) { return c.id == id; });
                // A malformed snapshot may repeat an ID under different keys.
                const bool duplicate = known != known_chats_.end() && known->cached_metadata;
                ChatItem chat = duplicate ? ChatItem{} : chatFromJson(value);
                chat.cached_metadata = true;
                if (!duplicate) chat.avatar_path = json_str(value, "@avatar_path");
                if (!duplicate && known != known_chats_.end() && known->avatar_path.empty() && known->avatar_file_id)
                    { known->avatar_path = chat.avatar_path; known->avatar_stale = !known->avatar_path.empty(); } // Until TDLib's download.
                if (!duplicate && known == known_chats_.end()) {
                    updateReadOutbox(id, json_i53(value, "last_read_outbox_message_id"));
                    known_chats_.push_back(chat);
                }
                for (const auto& entry : chat.list_orders) if (entry.second) {
                    auto copy = chat; copy.order = entry.second;
                    auto& rows = startup_chat_snapshots_[entry.first];
                    auto at = std::lower_bound(rows.begin(), rows.end(), copy, [](const ChatItem& a, const ChatItem& b) {
                        return a.order != b.order ? a.order > b.order : a.id > b.id;
                    });
                    rows.insert(at, std::move(copy));
                }
            } else if (cache_apply_stage_ == 2 && id && !user_names_.count(id)) {
                if (user_presence_.count(id)) json_object_del(value, "status");
                parseUser(value); user_photos_[id].fresh = false;
            }
            std::string key = json_object_iter_key(iter);
            json_object_del(collection, key.c_str());
            if (!json_object_size(collection)) { changed = changed || cache_apply_stage_ == 1; ++cache_apply_stage_; }
        } else {
            json_decref(snapshot); applying_snapshot_ = nullptr; changed = true; break;
        }
        if (sceKernelGetSystemTimeWide() - started >= (cache_apply_stage_ == 1 ? 8000 : 500)) break;
    }

    if (changed && cache_apply_stage_ != 1) publishChats();
    if (!ready_ && !chats_.empty() && input_kind_ == InputKind::None) status_ = i18n::text("conn.cached_connecting");
}

void TelegramClient::requestCachedDetails(int index) {
    // Online responses provide session-valid IDs. Offline details are read and
    // validated only when opened, on the same Storage worker as file checks.
    // Online, only a history the quick copy could not show (file not merged yet) is read here.
    if ((ready_ && index != -1) || logging_out_ || !cache_restore_allowed_) return;
    lock_queue(&queue_mutex_);
    cache_demand_ = {current_chat_id_, history_generation_, sticker_session_, sticker_generation_, index, true};
    unlock_queue(&queue_mutex_);
}
void TelegramClient::prepareCachedDetails() {
    lock_queue(&queue_mutex_);
    auto key = cache_demand_; cache_demand_.pending = false;
    unlock_queue(&queue_mutex_);
    if (!key.pending) return;
    if (key.index == -1 && ui_cache_.historyPending(key.chat)) {
        auto alive = [](void* opaque) -> bool { return !static_cast<TelegramClient*>(opaque)->stopping_; };
        ui_cache_.loadHistory(key.chat, alive, this);
    }
    json_t* data = key.index == -1 ? ui_cache_.history(key.chat) :
        key.index == -2 ? ui_cache_.stickerSnapshot() : ui_cache_.stickerItems(key.index);
    auto proceed = [](void* opaque) -> bool {
        auto* self = static_cast<TelegramClient*>(opaque);
        if (self->stopping_) return false;
        lock_queue(&self->queue_mutex_);
        const bool replaced = self->cache_demand_.pending;
        unlock_queue(&self->queue_mutex_);
        sceKernelDelayThread(500);
        return !replaced;
    };
    if (!media_cache_.resolve(data, true, proceed, this)) { if (data) json_decref(data); return; }
    clear_cached_file_ids(data);
    lock_queue(&queue_mutex_);
    if (!stopping_ && !cache_demand_.pending) {
        if (cache_result_) json_decref(static_cast<json_t*>(cache_result_));
        cache_result_ = data; cache_result_key_ = key; data = nullptr;
    }
    unlock_queue(&queue_mutex_);
    if (data) json_decref(data);
}
void TelegramClient::applyCachedDetails() {
    void* opaque = nullptr;
    lock_queue(&queue_mutex_);
    std::swap(opaque, cache_result_); auto key = cache_result_key_;
    unlock_queue(&queue_mutex_);
    auto* data = static_cast<json_t*>(opaque);
    if (!data) return;
    // Online, the cache only fills a chat whose first page has not arrived yet.
    const bool wanted = !ready_ || (key.index == -1 && history_pending_ && history_cursor_ == 0 &&
                                    (history_items_.empty() || history_cached_));
    if (wanted && !logging_out_ && cache_restore_allowed_ && key.chat == current_chat_id_ &&
        key.history == history_generation_ && key.session == sticker_session_ && key.stickers == sticker_generation_) {
        if (key.index == -1) {
            history_items_.clear();
            const char* id; json_t* message;
            json_object_foreach(data, id, message) if (matchesTopic(message)) history_items_.push_back(parseMessage(message));
            history_cached_ = !history_items_.empty(); rebuildHistory();
        } else if (key.index == -2) restoreStickerCache(data);
        else applyStickerResponse(data, false);
    }
    json_decref(data);
}
void TelegramClient::prepareCachedAvatar() {
    int64_t id = 0;
    lock_queue(&queue_mutex_);
    if (!cached_avatar_requests_.empty()) {
        id = cached_avatar_requests_.front(); cached_avatar_requests_.pop_front();
    }
    unlock_queue(&queue_mutex_);
    if (!id) return;
    auto* file = ui_cache_.chatAvatar(id);
    auto proceed = [](void* opaque) -> bool { return !static_cast<TelegramClient*>(opaque)->stopping_; };
    if (file && media_cache_.resolve(file, true, proceed, this)) {
        // Validate even descriptors without a remote cache key. Storage only.
        std::string path = valid_avatar_path(file);
        lock_queue(&queue_mutex_);
        if (!stopping_) cached_avatar_results_.emplace_back(id, std::move(path));
        unlock_queue(&queue_mutex_);
    }
    if (file) json_decref(file);
}
void TelegramClient::applyCachedAvatar() {
    std::pair<int64_t, std::string> result;
    lock_queue(&queue_mutex_);
    if (!cached_avatar_results_.empty()) {
        result = std::move(cached_avatar_results_.front()); cached_avatar_results_.pop_front();
    }
    unlock_queue(&queue_mutex_);
    if (!result.first || logging_out_ || !cache_restore_allowed_) return;
    auto apply = [&](ChatItem& chat) {
        if (chat.id == result.first && chat.cached_metadata) chat.avatar_path = result.second;
    };
    for (auto& chat : known_chats_) apply(chat);
    for (auto& chat : chats_) apply(chat);
    for (auto& entry : startup_chat_snapshots_) for (auto& chat : entry.second) apply(chat);
}

void TelegramClient::restoreStickerCache(void* opaque) {
    json_t* snapshot = static_cast<json_t*>(opaque);
    json_t* packs = json_object_get(snapshot, "sticker_packs");
    if (json_is_object(packs)) {
        clear_cached_file_ids(packs);
        sticker_packs_ = {{0, i18n::text("sticker.favorites")}, {1, i18n::text("sticker.recent")}};
        sticker_set_ids_ = {0, 0};
        applyStickerResponse(packs, true);
    }
    json_t* items = json_object_get(snapshot, "sticker_items");
    if (json_is_object(items)) {
        json_t* cached = json_object_get(items, "0");
        if (json_is_object(cached)) applyStickerResponse(cached, false);
    }
    sticker_status_ = sticker_packs_.size() > 2 ? i18n::text("sticker.choose_set") : sticker_status_;
}

void TelegramClient::applyStickerPackIcon(size_t index, void* opaque) {
    if (index >= sticker_packs_.size()) return;
    json_t* file = sticker_pack_preview(static_cast<json_t*>(opaque));
    auto& pack = sticker_packs_[index];
    pack.icon_file_id = (int32_t)json_i53(file, "id");
    json_t* local = json_object_get(file, "local");
    pack.icon_path = json_is_true(json_object_get(local, "is_downloading_completed")) ?
        json_str(local, "path") : "";
}

void TelegramClient::applyStickerResponse(void* opaque, bool packs) {
    json_t* root = static_cast<json_t*>(opaque);
    if (packs) {
        json_t* sets = json_object_get(root, "sets"); size_t i; json_t* set;
        json_array_foreach(sets, i, set) {
            ChatFolderItem pack;
            pack.id = (int32_t)sticker_packs_.size();
            pack.title = json_str(set, "title");
            sticker_packs_.push_back(pack);
            sticker_set_ids_.push_back(json_i53(set, "id"));
            applyStickerPackIcon(sticker_packs_.size()-1, set);
        }
        return;
    }
    if (sticker_apply_root_) json_decref(static_cast<json_t*>(sticker_apply_root_));
    sticker_apply_root_=json_incref(root);sticker_apply_offset_=0;sticker_apply_chat_=current_chat_id_;
    sticker_apply_generation_=sticker_generation_;sticker_apply_session_=sticker_session_;
    stickers_.clear();applyStickerSlice();
}

void TelegramClient::applyStickerSlice() {
    if (!sticker_apply_root_) return;
    auto* root=static_cast<json_t*>(sticker_apply_root_);
    if (sticker_apply_generation_!=sticker_generation_ || sticker_apply_session_!=sticker_session_ || sticker_apply_chat_!=current_chat_id_) {
        release_td_json(root);sticker_apply_root_=nullptr;return;
    }
    performance::SlowScope slow(performance::SlowPhase::Ui,"sticker_slice");
    auto* items=json_object_get(root,"stickers");const auto started=sceKernelGetSystemTimeWide();
    for (int n=0;n<8 && sticker_apply_offset_<json_array_size(items);++n) {
        auto* item=json_deep_copy(json_array_get(items,sticker_apply_offset_++));
        for (auto id:media_cache_.enqueue(item)) pending_files_.erase(id);
        media_cache_.resolve(item,false);
        SendStickerItem s;
        s.file = (int32_t)json_i53(json_object_get(item, "sticker"), "id");
        s.width = (int)json_i53(item, "width"); s.height = (int)json_i53(item, "height");
        s.emoji = json_str(item, "emoji");
        json_t* preview = json_object_get(json_object_get(item, "thumbnail"), "file");
        if (!std::strcmp(json_str(json_object_get(item, "format"), "@type"), "stickerFormatWebp")) preview = json_object_get(item, "sticker");
        s.preview = (int32_t)json_i53(preview, "id");
        json_t* local = json_object_get(preview, "local");
        if (json_is_true(json_object_get(local, "is_downloading_completed"))) s.path = json_str(local, "path");
        if (s.file || !s.path.empty()) stickers_.push_back(s);

        json_decref(item);
        if (sceKernelGetSystemTimeWide()-started>=500) break;
    }
    if (sticker_apply_offset_>=json_array_size(items)) {
        release_td_json(root);sticker_apply_root_=nullptr;
        sticker_status_=stickers_.empty() ? i18n::text("sticker.none_in_section") : "";
    } else sticker_status_=i18n::text("sticker.loading");
}

TelegramClient::TelegramClient()
    : client_(nullptr), ready_(false), closed_(false), requested_chats_(false),
      tdlib_parameters_sent_(false), input_kind_(InputKind::None), current_chat_id_(0) {}

TelegramClient::~TelegramClient() {
    stopWorker();
    if (cleanup_started_) pthread_join(cleanup_thread_, nullptr);
    if (sticker_apply_root_) json_decref(static_cast<json_t*>(sticker_apply_root_));
    pthread_mutex_destroy(&queue_mutex_);
}

void TelegramClient::stopWorker() {
    // The app exits through here: hand the list on screen to Storage before it stops.
    if (startup_list_dirty_ && cache_worker_started_) queueStartupList();
    stopping_ = true;

    const bool worker_running = worker_started_.exchange(false);
    const bool parser_running = parser_started_.exchange(false);
    const bool cache_running = cache_worker_started_.exchange(false);

    if (worker_running) pthread_join(worker_, nullptr);
    if (parser_running) pthread_join(parser_, nullptr);
    if (cache_running) pthread_join(cache_worker_, nullptr);
    sealer_.stop();
    media_cache_.invalidate(); // Never reuse late file jobs with a new TD session.
    if (applying_snapshot_) json_decref(static_cast<json_t*>(applying_snapshot_));
    applying_snapshot_ = nullptr;

    lock_queue(&queue_mutex_);
    for (void* root : parsed_) json_decref(static_cast<json_t*>(root));
    parsed_.clear(); incoming_.clear(); outgoing_.clear();
    if (restored_snapshot_) json_decref(static_cast<json_t*>(restored_snapshot_));
    restored_snapshot_ = nullptr;
    if (pending_startup_list_) json_decref(static_cast<json_t*>(pending_startup_list_));
    pending_startup_list_ = nullptr;
    if (cache_result_) json_decref(static_cast<json_t*>(cache_result_));
    cache_result_ = nullptr; cache_demand_.pending = false;
    cached_avatar_requests_.clear(); cached_avatar_results_.clear(); cached_avatar_seen_.clear();
    incoming_bytes_ = parsed_wire_bytes_ = 0; incoming_priority_ = 0;
    sampleQueues();
    unlock_queue(&queue_mutex_);
}

void* TelegramClient::workerEntry(void* self) {
    static_cast<TelegramClient*>(self)->runWorker();
    return nullptr;
}

void* TelegramClient::parserEntry(void* self) {
    static_cast<TelegramClient*>(self)->runParser();
    return nullptr;
}

void TelegramClient::sampleQueues() {
    performance::collector().queues(incoming_.size(), incoming_bytes_, parsed_.size(), parsed_wire_bytes_, outgoing_.size());
}

void TelegramClient::runParser() {
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Parser);
    int64_t slice_started = sceKernelGetSystemTimeWide();
    while (!stopping_) {
        std::string text;
        int64_t received_at = 0;
        lock_queue(&queue_mutex_);
        if (!incoming_.empty()) {
            text = std::move(incoming_.front().text);
            incoming_bytes_ -= text.size();
            received_at = incoming_.front().received;
            if (incoming_.front().priority && incoming_priority_) --incoming_priority_;
            incoming_.pop_front();
            sampleQueues();
        }
        unlock_queue(&queue_mutex_);
        if (text.empty()) { sceKernelDelayThread(2000); continue; }
        const auto raw_wait=performance::now()-received_at;
        performance::observe(performance::Metric::RawWait, raw_wait);
        performance::Scope parse_measurement(performance::Metric::Parse);
        performance::SlowScope slow_parse(performance::SlowPhase::Parse,"invalid_json",text.size());
        json_error_t error;
        json_t* root = compact_raw_unused_td_update(text);
        if (!root) root = compact_raw_sticker_sets(text);
        if (!root) root = compact_raw_chat_update(text);
        if (!root) root = compact_raw_sticker_items(text);
        const bool messages_candidate = !root && text.size() >= 2048 &&
            std::string_view(text.data(), 64).find("\"@type\":\"messages\"") != std::string_view::npos;
        if (messages_candidate) {
            const auto compact_started = performance::now();
            root = compact_raw_messages(text);
            performance::observe(performance::Metric::MessagesCompactUs, performance::now() - compact_started);
        }
        if (messages_candidate)
            performance::observe(root ? performance::Metric::MessagesCompactHit : performance::Metric::MessagesCompactMiss, (int64_t)text.size());
        // A big stickerSet/stickers reply that stays on Jansson means the raw
        // compaction rejected it; the log shows how often and how large.
        const bool sticker_candidate = !root && text.size() >= 2048 &&
            (std::string_view(text.data(), 128).find("\"stickerSet\"") != std::string_view::npos ||
             std::string_view(text.data(), 128).find("\"stickers\"") != std::string_view::npos);
        if (sticker_candidate) performance::observe(performance::Metric::StickerCompactMiss, (int64_t)text.size());
        if (!root) root = json_loads(text.c_str(), 0, &error);
        if (!root) {
            root = json_pack("{s:s,s:i,s:s}", "@type", "error", "code", 500,
                             "message", "Unable to parse TDLib response");
        }
        if (root) {
            json_object_set_new(root, "@received_at", json_integer(received_at));
            if (performance::enabled) json_object_set_new(root, "@wire_bytes", json_integer(text.size()));
            parse_measurement.finish();
            slow_parse.name(json_str(root,"@type")); slow_parse.finish();
            if (auto* compact=compact_unused_td_update(root)) {
                release_td_json(root);root=compact;
            }
            // Icon replies need one static preview, not a whole sticker set on UI.
            if (!std::strcmp(json_str(json_object_get(root,"@extra"),"kind"),"sticker_pack_icon") &&
                !std::strcmp(json_str(root,"@type"),"stickerSet")) {
                auto* compact=json_object();
                for (const char* field:{"@type","@extra","@received_at","@wire_bytes","id"})
                    json_object_set(compact,field,json_object_get(root,field));
                if (auto* preview=sticker_pack_preview(root))
                    json_object_set_new(compact,"thumbnail",json_pack("{s:{s:s},s:O}","format","@type","thumbnailFormatPng","file",preview));
                release_td_json(root);root=compact;
            }
            // The pack list needs only id, title and one static preview per set.
            // Trim the ~300 KB reply here so UI copy/save/release stay small.
            if (!std::strcmp(json_str(json_object_get(root,"@extra"),"kind"),"sticker_packs") &&
                !std::strcmp(json_str(root,"@type"),"stickerSets")) {
                auto* compact=json_object();
                for (const char* field:{"@type","@extra","@received_at","@wire_bytes"})
                    json_object_set(compact,field,json_object_get(root,field));
                auto* sets=json_array();
                size_t i; json_t* set;
                json_array_foreach(json_object_get(root,"sets"),i,set) {
                    auto* entry=json_pack("{s:s,s:O?,s:O?}","@type","stickerSetInfo","id",json_object_get(set,"id"),"title",json_object_get(set,"title"));
                    if (auto* preview=sticker_pack_preview(set))
                        json_object_set_new(entry,"thumbnail",json_pack("{s:{s:s},s:O}","format","@type","thumbnailFormatPng","file",preview));
                    if (entry) json_array_append_new(sets,entry);
                }
                json_object_set_new(compact,"sets",sets);
                release_td_json(root);root=compact;
            }
            // Deliver immediately: filesystem validation must never hold up the
            // serial protocol stream. The UI queues file-only copies separately.
            // Transfer ownership: only the UI may access/decref this object next.
            auto* diagnostic_extra=json_object_get(root,"@extra");
            const bool diagnostic_history=!std::strcmp(json_str(diagnostic_extra,"kind"),"history") && json_is_true(json_object_get(diagnostic_extra,"first"));
            const bool diagnostic_forum=!std::strcmp(json_str(diagnostic_extra,"kind"),"forum_check");
            const auto sent_at=json_i53(diagnostic_extra,"send_at");
            if ((diagnostic_history || diagnostic_forum) && sent_at>0 && received_at>=sent_at)
                performance::observe(diagnostic_history ? performance::Metric::FirstHistoryResponse : performance::Metric::ForumResponse,received_at-sent_at);
            if (diagnostic_history) performance::observe(performance::Metric::FirstHistoryParse,performance::now()-received_at-raw_wait);
            if (diagnostic_history || (!std::strcmp(json_str(diagnostic_extra,"kind"),"history") && !std::strcmp(json_str(root,"@type"),"error")))
                foreground_until_.store(0);
            auto metadata=describe_td_event(root);
            if (!std::strncmp(json_str(diagnostic_extra, "kind"), "gif_", 4))
                gif_diagnostics::record("parsed", json_str(diagnostic_extra, "kind"), json_i53(diagnostic_extra, "generation"), performance::now());
            if (metadata.history && json_is_true(json_object_get(json_object_get(root,"@extra"),"first"))) {
                performance::observe(performance::Metric::FirstHistoryRawWait,raw_wait);
                metadata.parsed=performance::now();
                json_object_set_new(root,"@parsed_at",json_integer(metadata.parsed));
            }
            // Persisting a history reply copies every message (50-150 ms for 80 KB);
            // do it here, off the UI thread, while this thread still owns the tree.
            if (!logging_out_ && !std::strcmp(json_str(root,"@type"),"messages")) {
                ui_cache_.rememberMessages(root,text.size());
                json_object_set_new(root,"@ui_remembered",json_true());
            }
            // The pack list copy (~100 ms under load) belongs on this thread too.
            if (!logging_out_ && !std::strcmp(json_str(root,"@type"),"stickerSets") &&
                !std::strcmp(json_str(json_object_get(root,"@extra"),"kind"),"sticker_packs")) {
                ui_cache_.saveStickerPacks(root);
                json_object_set_new(root,"@ui_saved",json_true());
            }
            lock_queue(&queue_mutex_);
            parsed_wire_bytes_ += metadata.bytes;
            parsed_wire_bytes_ -= parsed_.enqueue(root,metadata);
            sampleQueues();
            unlock_queue(&queue_mutex_);
        }
        if (sceKernelGetSystemTimeWide() - slice_started >= 2000) {
            sceKernelDelayThread(500);
            slice_started = sceKernelGetSystemTimeWide();
        }
    }
}

bool TelegramClient::cacheLoadProceed(void* opaque) {
    auto* self = static_cast<TelegramClient*>(opaque);
    int64_t pause_started=0;
    while (!self->stopping_) {
        lock_queue(&self->queue_mutex_);
        const size_t pending = self->incoming_.size() + self->parsed_.size();
        // Live events waiting for the Parser/UI mean the cores are shared with
        // this load: halve its duty cycle (2 ms work, 2 ms rest) instead of 80%.
        // A large backlog stops it: running alongside the startup update flood
        // slowed the Parser (10 s event backlog), the UI frames and first chat
        // opening, while the load itself finished no sooner.
        const bool congested = pending >= 32;
        const bool busy = pending >= 8;
        unlock_queue(&self->queue_mutex_);
        const auto now = sceKernelGetSystemTimeWide();
        if (now>=self->interaction_until_.load() && now>=self->cache_service_at_) {
            self->cache_service_at_=now+10000;
            self->prepareCachedAvatar();
            auto permitted=[](void* opaque)->bool {
                auto* client=static_cast<TelegramClient*>(opaque);
                return !client->stopping_ && sceKernelGetSystemTimeWide()>=client->interaction_until_.load();
            };
            const auto started=performance::now();
            if (self->media_cache_.validateNext(permitted,self))
                performance::observe(performance::Metric::CacheService,performance::now()-started);
        }
        const auto after_service=sceKernelGetSystemTimeWide();
        if (!congested && !self->uploads_pending_ && after_service >= self->interaction_until_.load()) {
            if (now-self->cache_load_slice_ >= 2000) {
                sceKernelDelayThread(busy ? 2000 : 500); self->cache_load_slice_ = sceKernelGetSystemTimeWide();
            }
            if (pause_started) performance::observe(performance::Metric::CachePause,performance::now()-pause_started);
            return !self->stopping_;
        }
        if (!pause_started) pause_started=performance::now();
        sceKernelDelayThread(2000);
    }
    if (pause_started) performance::observe(performance::Metric::CachePause,performance::now()-pause_started);
    return false;
}

void* TelegramClient::cacheEntry(void* opaque) {
    auto* self = static_cast<TelegramClient*>(opaque);
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Storage);
    if (self->restore_on_cache_worker_) {
        auto* snapshot = static_cast<json_t*>(self->prepareCache());
        lock_queue(&self->queue_mutex_);
        if (!self->stopping_) { self->restored_snapshot_ = snapshot; snapshot = nullptr; }
        self->startup_index_read_ = true;
        unlock_queue(&self->queue_mutex_);
        if (snapshot) json_decref(snapshot);
        if (!self->stopping_) {
            performance::Scope details(performance::Metric::CacheDetailsLoad);
            if (!self->full_cache_loaded_) { self->ui_cache_.load(cacheLoadProceed, self); self->full_cache_loaded_ = true; }
            self->media_cache_.load();
        }
    }
    int64_t next = sceKernelGetSystemTimeWide() + 30000000;
    int64_t next_performance = performance::now() + 5000000;
    // Leave the card to startup and the first chat list before sweeping.
    const int64_t sweep_after = sceKernelGetSystemTimeWide() + 20000000;
    // A slow card must not be worn for the whole session: sweep in a short window only.
    const int64_t sweep_until = sweep_after + 40000000;
    auto proceed = [](void* opaque) -> bool {
        auto* self = static_cast<TelegramClient*>(opaque);
        return !self->stopping_ && !self->uploads_pending_ && !cpu_scheduler::videoActive() &&
            sceKernelGetSystemTimeWide() >= self->interaction_until_.load();
    };
    while (!self->stopping_) {
        // Media and cache files share the same Vita storage device. Delay
        // validation, directory sweeps and snapshot writes during playback.
        if (cpu_scheduler::videoActive()) { sceKernelDelayThread(100000); continue; }
        int64_t now = sceKernelGetSystemTimeWide();
        if (now < self->interaction_until_.load()) { sceKernelDelayThread(10000); continue; }
        self->prepareCachedDetails();
        self->prepareCachedAvatar();
        self->writeStartupList(false);
        // Chat histories are merged one file per pass, the open chat first.
        const bool merged = self->full_cache_loaded_ &&
            self->ui_cache_.loadHistory(self->history_preferred_.load(), cacheLoadProceed, self);
        auto validate = [](void* opaque) -> bool {
            auto* client = static_cast<TelegramClient*>(opaque);
            return !client->stopping_ && !cpu_scheduler::videoActive() &&
                sceKernelGetSystemTimeWide() >= client->interaction_until_.load();
        };
        const bool checked_file = self->media_cache_.validateNext(validate, self);
        // Old media directories are emptied only while nothing interactive waits.
        if (!checked_file && validate(self) && !self->uploads_pending_ &&
            now >= sweep_after && now < sweep_until && now >= self->foreground_until_.load())
            self->trash_.step(now); // Self-throttled; the idle 100 ms sleep below is enough.
        self->flushUploadLogs();
        if (now >= next_performance) {
            gif_diagnostics::flush();
            const int64_t sampled = performance::now();
            if (diag_log::enabled(diag_log::Kind::Performance))
                performance::flush("ux0:data/vitagram/performance.log", "ux0:data/vitagram/performance.previous.log", VITAGRAM_VERSION, sampled);
            next_performance = sceKernelGetSystemTimeWide() + 5000000;
        }
        if (now >= next) {
            lock_queue(&self->queue_mutex_);
            bool congested = self->incoming_.size()+self->parsed_.size() >= 128;
            unlock_queue(&self->queue_mutex_);
            if (!congested) {
                self->media_cache_.flush(proceed, self);
                self->ui_cache_.flush(proceed, self);
                next = sceKernelGetSystemTimeWide() + 90000000;
            }
        }
        sceKernelDelayThread(checked_file || merged ? 2000 : 100000);
    }
    // Caches are best-effort: keep the last atomic snapshot instead of blocking
    // shutdown with a fresh full save. An in-flight write observes stopping_.
    // The startup index is small: the list shown at exit is the first one shown next time.
    self->writeStartupList(true);
    self->flushUploadLogs();
    gif_diagnostics::flush();
    if (diag_log::enabled(diag_log::Kind::Performance))
        performance::flush("ux0:data/vitagram/performance.log", "ux0:data/vitagram/performance.previous.log", VITAGRAM_VERSION, performance::now());
    return nullptr;
}

void TelegramClient::runWorker() {
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Network);
    // This port's TDLib scheduler is single-threaded: keep every TD call here.
    // Callback registration doesn't change verbosity. The TDLib default is 5
    // and formats/writes detailed protocol logs synchronously on this thread.
    td_json_client_execute(nullptr, "{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":1}");
    // TDLib must not hold files in a directory while it is swapped out.
    media_trash::rotate();
    client_ = td_json_client_create();
    if (!client_) {
        lock_queue(&queue_mutex_);
        incoming_.push_back({"{\"@type\":\"error\",\"code\":500,\"message\":\"TDLib creation failed\"}", sceKernelGetSystemTimeWide()});
        incoming_bytes_ += incoming_.back().text.size(); sampleQueues();
        unlock_queue(&queue_mutex_);
        return;
    }
    int64_t last_receive = 0;
    while (!stopping_) {
        std::deque<Request> requests;
        lock_queue(&queue_mutex_);
        // Hard memory limit: over it, TDLib is neither fed nor run until the queues drain (see
        // td_queue_paused). Parser and UI keep consuming meanwhile.
        const bool was_paused = receive_paused_;
        receive_paused_ = td_queue_paused(receive_paused_, incoming_bytes_ + parsed_wire_bytes_, incoming_.size() + parsed_.size());
        if (receive_paused_) {
            const size_t queued_bytes = incoming_bytes_ + parsed_wire_bytes_, queued_events = incoming_.size() + parsed_.size();
            unlock_queue(&queue_mutex_);
            if (!was_paused) std::fprintf(stderr, "TDLib paused: %zu bytes / %zu events queued\n", queued_bytes, queued_events);
            sceKernelDelayThread(5000);
            continue;
        }
        // Bound each send batch; receiving updates and yielding must not be
        // postponed behind a large burst of thumbnail/history requests.
        // Оптимизация: увеличиваем буфер с 128 до 200 запросов для более быстрой загрузки чатов
        const bool congested = incoming_.size() + parsed_.size() >= 200;
        const bool interactive = sceKernelGetSystemTimeWide() < interaction_until_.load();
        bool foreground = sceKernelGetSystemTimeWide() < foreground_until_.load();
        for (int i = 0; i < (interactive ? 1 : request_batch_size_.load()) && !outgoing_.empty(); ++i) {
            int64_t now = sceKernelGetSystemTimeWide();
            auto best = outgoing_.end();
            auto score = [&](const Request& request) {
                // Aging prevents background requests from starving indefinitely.
                return request.priority + std::min<int64_t>(4, (now-request.queued)/2000000);
            };
            for (auto it = outgoing_.begin(); it != outgoing_.end(); ++it) {
                if (!td_request_allowed(it->priority,it->backlog_exempt,congested,interactive,foreground,now-it->queued)) continue;
                if (best == outgoing_.end() || score(*it) > score(*best)) best = it;
            }
            if (best == outgoing_.end()) break;
            performance::observe(performance::Metric::RequestWait, now-best->queued);
            if (best->first_history) {
                performance::observe(performance::Metric::FirstHistoryRequestWait,now-best->queued);
                // Until the reply arrives, background downloads stay in the app queue.
                foreground_until_.store(now + 10000000); foreground = true;
            }
            if (best->forum_check) performance::observe(performance::Metric::ForumRequestWait,now-best->queued);
            best->selected=now;
            requests.push_back(std::move(*best)); outgoing_.erase(best);
        }
        sampleQueues();
        unlock_queue(&queue_mutex_);
        for (auto& request : requests) {
            // Only tracked requests are stamped, outside the shared mutex.
            // @extra is echoed by TDLib, correlating the reply without a map.
            const bool tracked=performance::enabled && (request.first_history || request.forum_check);
            json_t* stamped=nullptr;
            if (tracked) {
                stamped=json_loads(request.text.c_str(),0,nullptr);
                auto* extra=json_object_get(stamped,"@extra");
                if (json_is_object(extra)) {
                    json_object_set_new(extra,"selected_at",json_integer(request.selected));
                    json_object_set_new(extra,"send_at",json_integer(performance::now()));
                    char* dumped=json_dumps(stamped,JSON_COMPACT | JSON_ENSURE_ASCII);
                    if (dumped) { request.text=dumped;std::free(dumped); }
                }
                json_decref(stamped);
            }
            const auto send_started=performance::now();
            gif_diagnostics::raw("sent", request.text.c_str(), send_started);
            { performance::Scope measurement(performance::Metric::Send);
              td_json_client_send(client_, request.text.c_str()); }
            if (tracked) performance::observe(request.first_history ? performance::Metric::FirstHistorySend : performance::Metric::ForumSend,performance::now()-send_started);
        }
        bool received = false;
        // In the single-threaded Vita port receive() ALSO advances socket I/O.
        // Backpressure new requests, never the scheduler. Drain response bursts
        // within a time budget; the parser coalesces file-progress snapshots.
        int64_t receive_started = sceKernelGetSystemTimeWide();
        for (int n = 0; n < 32; ++n) {
            int64_t receive_time = performance::now();
            if (last_receive) performance::observe(performance::Metric::ReceiveGap, receive_time-last_receive);
            last_receive = receive_time;
            const char* response = td_json_client_receive(client_, 0.0);
            performance::observe(performance::Metric::Receive, performance::now()-receive_time);
            if (response) {
                gif_diagnostics::raw("received", response, receive_time);
                lock_queue(&queue_mutex_);
                if ((incoming_bytes_ + parsed_wire_bytes_ > kTdQueueHighWaterBytes ||
                     incoming_.size() + parsed_.size() > kTdQueueHighWaterEvents) && td_event_is_ephemeral(response)) {
                    // Overloaded: shed a value the next update of the same kind replaces anyway.
                    ++shed_events_;
                    const int64_t now = sceKernelGetSystemTimeWide();
                    const bool report = now - shed_reported_ >= 10000000;
                    if (report) shed_reported_ = now;
                    const uint64_t shed = shed_events_;
                    unlock_queue(&queue_mutex_);
                    if (report) std::fprintf(stderr, "TDLib queue over budget: %llu ephemeral updates dropped so far\n", (unsigned long long)shed);
                    received = true;
                    continue;
                }
                // Replies to the open chat (history, reactions, forum) jump the
                // startup update flood; TDLib writes "@extra" at the end.
                const bool priority = td_reply_is_foreground(response);
                Incoming entry{response, sceKernelGetSystemTimeWide(), priority};
                incoming_bytes_ += entry.text.size();
                if (priority) { incoming_.insert(incoming_.begin() + incoming_priority_, std::move(entry)); ++incoming_priority_; }
                else incoming_.push_back(std::move(entry));
                sampleQueues();
                unlock_queue(&queue_mutex_);
                received = true;
            } else break;
            if (sceKernelGetSystemTimeWide() - receive_started >= 2000) break;
        }
        sceKernelDelayThread(received ? 500 : 2000);
    }
    td_json_client_destroy(client_);
    client_ = nullptr;
}

bool TelegramClient::start(bool defer_cache_worker) {
    if (worker_started_) return true;
    if (VITAGRAM_API_ID <= 0 || std::strstr(VITAGRAM_API_HASH, "PUT_YOUR")) {
        error_ = i18n::text("cfg.edit");
        status_ = i18n::text("cfg.required");
        return false;
    }

    if (account_cleanup::pending() && !account_cleanup::purge()) {
        error_ = status_ = i18n::text("account.cleanup_blocked");
        return false;
    }
    sceIoMkdir("ux0:data/vitagram", 0777);
    sceIoMkdir(VITAGRAM_DB_DIR, 0777);
    sceIoMkdir(VITAGRAM_FILES_DIR, 0777);

    td_set_log_message_callback(1, nullptr);
    pthread_attr_t attributes;
    stopping_ = false;
    pthread_attr_init(&attributes);
    // Оптимизация: уменьшение размера стека с 2МБ до 1МБ экономит ~3-4МБ RAM (3 потока)
    pthread_attr_setstacksize(&attributes, 1 * 1024 * 1024);
    int worker_result = pthread_create(&worker_, &attributes, workerEntry, this);
    int parser_result = worker_result == 0 ? pthread_create(&parser_, &attributes, parserEntry, this) : -1;
    pthread_attr_destroy(&attributes);
    worker_started_ = worker_result == 0;
    parser_started_ = parser_result == 0;
    if (!worker_started_ || !parser_started_) {
        error_ = i18n::text("tg.worker_failed");
        stopWorker();
        return false;
    }
    if (!defer_cache_worker && !startCacheWorker()) { stopWorker(); return false; }
    sealer_.start(); // seals media as TDLib finishes downloading it (vault on only)
    status_ = i18n::text("tg.created_waiting");
    sendRaw("{\"@type\":\"getAuthorizationState\",\"@extra\":\"startup_probe\"}");
    return true;
}

bool TelegramClient::startCacheWorker() {
    if (cache_worker_started_) return true;
    if (stopping_) return false;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    // Оптимизация: уменьшение размера стека с 2МБ до 1МБ экономит ~3-4МБ RAM (3 потока)
    pthread_attr_setstacksize(&attributes, 1 * 1024 * 1024);
    int result = pthread_create(&cache_worker_, &attributes, cacheEntry, this);
    pthread_attr_destroy(&attributes);
    cache_worker_started_ = result == 0;
    if (result != 0) {
        error_ = i18n::text("cache.worker_failed");
        notice_ = error_; notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
        std::fprintf(stderr, "%s: %d\n", error_.c_str(), result);
    }
    return result == 0;
}

namespace {
json_t* proxy_json(const ProxyConfig& cfg) {
    json_t* type;
    if (cfg.kind == ProxyKind::Socks5)
        type = json_pack("{s:s,s:s,s:s}", "@type", "proxyTypeSocks5", "username", cfg.username.c_str(),
                         "password", cfg.password.c_str());
    else if (cfg.kind == ProxyKind::Https)
        type = json_pack("{s:s,s:s,s:s,s:b}", "@type", "proxyTypeHttp", "username", cfg.username.c_str(),
                         "password", cfg.password.c_str(), "http_only", false);
    else
        type = json_pack("{s:s,s:s}", "@type", "proxyTypeMtproto", "secret", cfg.secret.c_str());
    return json_pack("{s:s,s:s,s:i,s:o}", "@type", "proxy", "server", cfg.server.c_str(), "port", cfg.port,
                     "type", type);
}
}

void TelegramClient::configureProxy(bool enabled, const ProxyConfig& proxy) {
    cancelProxyCheck();
    proxy_enabled_ = enabled; proxy_ = proxy;
    if (worker_started_ && tdlib_parameters_sent_) applyProxy();
}

namespace {
const char* const kMarkerConsole = VITAGRAM_DB_DIR "/.encrypted-v1";
const char* const kMarkerVault = VITAGRAM_DB_DIR "/.encrypted-v2";
bool marker_exists(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}
}

int TelegramClient::databaseMarker() const {
    return marker_exists(kMarkerVault) ? 2 : marker_exists(kMarkerConsole) ? 1 : 0;
}

int TelegramClient::targetDatabaseMarker() const {
    if (vault::enabled() && !vault::disabling()) return 2;
    return secure_store::database_key().empty() ? 0 : 1;
}

std::string TelegramClient::currentDatabaseKey() const {
    switch (databaseMarker()) {
        case 2: return vault::database_key();
        case 1: return secure_store::database_key();
        default: return std::string();
    }
}

std::string TelegramClient::targetDatabaseKey() const {
    return targetDatabaseMarker() == 2 ? vault::database_key() : secure_store::database_key();
}

bool TelegramClient::databaseKeyCurrent() const {
    return !database_key_migrating_ && databaseMarker() == targetDatabaseMarker();
}

// Re-keys the database once: new installs and updates start unencrypted (or under the console
// key), and the vault switches the key when it is turned on or off. The marker lives inside the
// database directory so logout cleanup removes both together.
void TelegramClient::migrateDatabaseKey() {
    if (database_key_migrating_ || databaseMarker() == targetDatabaseMarker()) return;
    const int target = targetDatabaseMarker();
    if (target == 2 && !vault::unlocked()) return;
    database_key_migrating_ = true;
    auto* request = json_pack("{s:s,s:s}", "@type", "setDatabaseEncryptionKey", "new_encryption_key", targetDatabaseKey().c_str());
    json_object_set_new(request, "@extra", json_pack("{s:s,s:i}", "kind", "db_key", "marker", target));
    sendJson(request);
}

void TelegramClient::applyProxy() {
    json_t* request;
    if (proxy_enabled_) {
        request = json_pack("{s:s,s:o,s:b,s:s}", "@type", "addProxy", "proxy", proxy_json(proxy_), "enable", true,
                            "comment", "VitaGram");
    } else request = json_pack("{s:s}", "@type", "disableProxy");
    json_object_set_new(request, "@extra", json_pack("{s:s,s:i}", "kind", "proxy_config", "generation", ++proxy_generation_));
    proxy_status_ = i18n::text("proxy.applying");
    sendJson(request);
}

void TelegramClient::cancelProxyCheck() {
    ++proxy_check_generation_;
    proxy_check_deadline_ = 0; proxy_check_status_.clear();
}

void TelegramClient::checkProxy(const ProxyConfig& proxy) {
    if (proxy_check_deadline_) return;
    cancelProxyCheck();
    if (!proxy.complete()) {
        proxy_check_status_ = i18n::text("proxy.need_fields"); return;
    }
    if (!worker_started_ || !tdlib_parameters_sent_) {
        proxy_check_status_ = i18n::text("proxy.check.starting"); return;
    }
    auto* request = json_pack("{s:s,s:o}", "@type", "pingProxy", "proxy", proxy_json(proxy));
    json_object_set_new(request, "@extra", json_pack("{s:s,s:i}", "kind", "proxy_check", "generation", proxy_check_generation_));
    proxy_check_status_ = i18n::text("proxy.checking");
    proxy_check_deadline_ = sceKernelGetSystemTimeWide() + 15000000;
    sendJson(request);
}

void TelegramClient::sendRaw(const char* json, int priority, bool first_history, bool backlog_exempt, int32_t file_id, bool forum_check) {
    if (!worker_started_) return;
    lock_queue(&queue_mutex_);
    outgoing_.push_back({json, priority, sceKernelGetSystemTimeWide(),first_history,backlog_exempt,file_id,forum_check});
    sampleQueues();
    unlock_queue(&queue_mutex_);
}

void TelegramClient::sendJson(void* opaque) {
    json_t* root = (json_t*)opaque;
    const char* method = json_str(root, "@type");
    if ((!std::strcmp(method, "sendMessage") || !std::strcmp(method, "sendInlineQueryResultMessage")) && current_topic_ && !json_object_get(root, "topic_id"))
        json_object_set_new(root, "topic_id", json_pack("{s:s,s:i}", "@type", "messageTopicForum", "forum_topic_id", current_topic_));
    // Anything sent from the comments view goes into that thread (media senders pass a null topic).
    if (current_thread_ && json_i53(root, "chat_id") == current_chat_id_ &&
        (!std::strcmp(method, "sendMessage") || !std::strcmp(method, "sendInlineQueryResultMessage") || !std::strcmp(method, "sendMessageAlbum") || !std::strcmp(method, "forwardMessages")) &&
        !json_is_object(json_object_get(root, "topic_id")))
        json_object_set_new(root, "topic_id", json_pack("{s:s,s:I}", "@type", "messageTopicThread", "message_thread_id", (json_int_t)current_thread_));
    const char* kind = json_str(json_object_get(root, "@extra"), "kind");
    if (!std::strncmp(kind, "gif_", 4)) {
        gif_request_kind_ = kind;
        gifs_.begin(sceKernelGetSystemTimeWide());
        gif_diagnostics::record("queued", kind, gifs_.generation, performance::now());
    }
    if (!std::strcmp(kind, "media_upload")) {
        auto* options = json_object_get(root, "options");
        if (!json_is_object(options)) {
            options = json_pack("{s:s}", "@type", "messageSendOptions");
            json_object_set_new(root, "options", options);
        }
        json_object_set_new(options, "sending_id", json_integer(json_i53(json_object_get(root, "@extra"), "token")));
    }
    int priority = 1;
    if (!std::strncmp(kind, "gif_", 4) || !std::strcmp(method, "sendInlineQueryResultMessage")) priority = 4;
    if (!std::strncmp(kind, "poll_", 5) || !std::strncmp(kind, "reaction_", 9) || !std::strncmp(kind, "sticker_", 8) || !std::strncmp(kind, "forum_", 6) || !std::strcmp(kind, "proxy_check")) priority = 4;
    if (!std::strcmp(method, "getChatHistory") || !std::strcmp(method, "getForumTopicHistory") || !std::strcmp(method, "getMessageThreadHistory") || !std::strcmp(method, "getForumTopics")) priority = 9;
    else if (!std::strcmp(method, "downloadFile")) priority = json_i53(root, "priority") >= 32 ? 4 : 1;
    else if (!std::strcmp(method, "cancelDownloadFile") || !std::strcmp(method, "sendMessage") ||
             !std::strcmp(method, "openChat") || !std::strcmp(method, "closeChat")) priority = 4;
    else if (!std::strncmp(kind, "refresh_main", 12)) priority = refresh_manual_ ? 3 : 0;
    else if (!std::strncmp(kind,"post_",5)) priority = 4;
    else if (!std::strcmp(kind,"avatar_metadata")) priority = 3;
    else if (!std::strcmp(kind,"sticker_pack_icon") || !std::strcmp(kind,"sticker_packs")) priority = 1;
    // Only the displayed list loads chats (never while a chat is open): it is
    // what the user looks at, so it must pass the interaction gate.
    else if (!std::strcmp(method, "loadChats")) priority = 5;
    else if (!std::strcmp(method, "optimizeStorage")) priority = 0;
    if (!std::strcmp(method,"openChat") || !std::strcmp(method,"closeChat")) priority=10;
    if (!std::strcmp(kind,"forum_check")) priority=9;
    char* dumped = json_dumps(root, JSON_COMPACT | JSON_ENSURE_ASCII);
    if (dumped) {
        sendRaw(dumped, priority,json_is_true(json_object_get(json_object_get(root,"@extra"),"first")),
            !std::strcmp(method,"loadChats") || !std::strcmp(kind,"visible_avatar") || !std::strcmp(kind,"avatar_metadata"),!std::strcmp(method,"downloadFile") ? (int32_t)json_i53(root,"file_id") : 0,!std::strcmp(kind,"forum_check"));
        std::free(dumped);
    }
    json_decref(root);
}

void TelegramClient::setInput(InputKind kind, const i18n::Text& status) {
    input_kind_ = kind;
    status_ = status;
}

std::string TelegramClient::inputPrompt() const {
    switch (input_kind_) {
        case InputKind::Phone: return tr("input.phone");
        case InputKind::Code: return tr("input.code");
        case InputKind::Password: return tr("input.password");
        case InputKind::FirstName: return tr("input.first_name");
        case InputKind::Email: return tr("input.email");
        case InputKind::EmailCode: return tr("input.email_code");
        case InputKind::Message: return tr("input.message");
        default: return "";
    }
}

bool TelegramClient::canResendCode() const {
    return input_kind_ == InputKind::Code && auth_can_resend_ && sceKernelGetSystemTimeWide() >= auth_resend_at_;
}

std::string TelegramClient::authResendInfo() const {
    if (input_kind_ != InputKind::Code || !auth_can_resend_) return std::string();
    int64_t left = (auth_resend_at_ - sceKernelGetSystemTimeWide() + 999999) / 1000000;
    return left > 0 ? i18n::text("auth.resend_wait", {auth_next_via_, std::to_string(left)}) : i18n::text("auth.resend_ready", {auth_next_via_});
}

void TelegramClient::requestQr() {
    if (!canRequestQr()) return;
    const InputKind previous = input_kind_;
    input_kind_ = InputKind::None;
    auth_detail_.clear(); auth_can_resend_ = false;
    status_ = i18n::text("auth.qr_requesting");
    // A failure puts the sign-in prompt back (see the auth_input handler).
    sendJson(json_pack("{s:s,s:[],s:{s:s,s:i}}", "@type", "requestQrCodeAuthentication", "other_user_ids",
                       "@extra", "kind", "auth_input", "input", (int)previous));
}

void TelegramClient::backInput() {
    // TDLib accepts a new phone number while it waits for the code, password or name.
    if (!canGoBackInput()) return;
    auth_detail_.clear(); auth_can_resend_ = false;
    setInput(InputKind::Phone, i18n::text("auth.prompt.phone"));
}

void TelegramClient::resendCode() {
    if (!canResendCode()) return;
    auth_can_resend_ = false;
    status_ = i18n::text("auth.sending_code");
    sendJson(json_pack("{s:s}", "@type", "resendAuthenticationCode"));
}

void TelegramClient::cancelInput() {
    if (input_kind_ == InputKind::Message) { input_kind_ = InputKind::None; reply_target_ = 0; }
}

void TelegramClient::submitInput(const std::string& value) {
    if (value.empty()) return;
    InputKind kind = input_kind_;
    input_kind_ = InputKind::None;

    json_t* root = nullptr;
    if (kind == InputKind::Phone) {
        root = json_object();
        json_object_set_new(root, "@type", json_string("setAuthenticationPhoneNumber"));
        json_object_set_new(root, "phone_number", json_string(value.c_str()));
        json_t* settings = json_object();
        json_object_set_new(settings, "@type", json_string("phoneNumberAuthenticationSettings"));
        json_object_set_new(settings, "allow_flash_call", json_false());
        json_object_set_new(settings, "allow_missed_call", json_false());
        json_object_set_new(settings, "is_current_phone_number", json_false());
        json_object_set_new(settings, "has_unknown_phone_number", json_false());
        json_object_set_new(settings, "allow_sms_retriever_api", json_false());
        json_object_set_new(settings, "firebase_authentication_settings", json_null());
        json_object_set_new(settings, "authentication_tokens", json_array());
        json_object_set_new(root, "settings", settings);
        status_ = i18n::text("auth.sending_code");
    } else if (kind == InputKind::Code) {
        root = json_pack("{s:s,s:s}", "@type", "checkAuthenticationCode", "code", value.c_str());
        status_ = i18n::text("auth.checking_code");
    } else if (kind == InputKind::Password) {
        root = json_pack("{s:s,s:s}", "@type", "checkAuthenticationPassword", "password", value.c_str());
        status_ = i18n::text("auth.checking_password");
    } else if (kind == InputKind::FirstName) {
        root = json_pack("{s:s,s:s,s:s}", "@type", "registerUser", "first_name", value.c_str(), "last_name", "");
        status_ = i18n::text("auth.registering");
    } else if (kind == InputKind::Email) {
        root = json_pack("{s:s,s:s}", "@type", "setAuthenticationEmailAddress", "email_address", value.c_str());
        status_ = i18n::text("auth.sending_code");
    } else if (kind == InputKind::EmailCode) {
        root = json_pack("{s:s,s:{s:s,s:s}}", "@type", "checkAuthenticationEmailCode",
                         "code", "@type", "emailAddressAuthenticationCode", "code", value.c_str());
        status_ = i18n::text("auth.checking_code");
    } else if (kind == InputKind::Message && current_chat_id_ != 0) {
        root = json_object();
        json_object_set_new(root, "@type", json_string("sendMessage"));
        json_object_set_new(root, "@extra", json_string("send_message"));
        json_object_set_new(root, "chat_id", json_integer(current_chat_id_));
        json_object_set_new(root, "message_thread_id", json_integer(0));
        json_object_set_new(root, "reply_to_message_id", json_integer(0));
        if (reply_target_) {
            json_object_set_new(root, "reply_to", json_pack("{s:s,s:I}", "@type", "inputMessageReplyToMessage", "message_id", (json_int_t)reply_target_));
            reply_target_ = 0;
        }
        json_object_set_new(root, "options", json_null());
        json_object_set_new(root, "reply_markup", json_null());

        json_t* content = json_object();
        json_object_set_new(content, "@type", json_string("inputMessageText"));
        json_t* ft = json_object();
        json_object_set_new(ft, "@type", json_string("formattedText"));
        json_object_set_new(ft, "text", json_string(value.c_str()));
        json_object_set_new(ft, "entities", json_array());
        json_object_set_new(content, "text", ft);
        json_object_set_new(content, "disable_web_page_preview", json_false());
        json_object_set_new(content, "clear_draft", json_true());
        json_object_set_new(root, "input_message_content", content);
        status_ = i18n::text("common.sending");
    }

    if (root && kind != InputKind::Message)
        json_object_set_new(root, "@extra", json_pack("{s:s,s:i}", "kind", "auth_input", "input", (int)kind));
    if (root) sendJson(root);
}

void TelegramClient::processAuth(void* opaque) {
    json_t* root = (json_t*)opaque;
    json_t* state = json_object_get(root, "authorization_state");
    if (!json_is_object(state)) state = root;
    const char* type = json_str(state, "@type");
    if (std::strcmp(type, "authorizationStateWaitOtherDeviceConfirmation")) qr_link_.clear();

    if (!std::strcmp(type, "authorizationStateWaitTdlibParameters")) {
        // Both the unsolicited update and our startup probe can report this state.
        // TDLib accepts setTdlibParameters only once for a given transition.
        if (tdlib_parameters_sent_) return;
        tdlib_parameters_sent_ = true;
        json_t* params = json_object();
        json_object_set_new(params, "@type", json_string("setTdlibParameters"));
        json_object_set_new(params, "use_test_dc", json_false());
        json_object_set_new(params, "database_directory", json_string(VITAGRAM_DB_DIR));
        json_object_set_new(params, "files_directory", json_string(VITAGRAM_FILES_DIR));
        json_object_set_new(params, "database_encryption_key", json_string(currentDatabaseKey().c_str()));
        json_object_set_new(params, "@extra", json_pack("{s:s}", "kind", "tdlib_params"));
        json_object_set_new(params, "use_file_database", json_false());
        json_object_set_new(params, "use_chat_info_database", json_false());
        json_object_set_new(params, "use_message_database", json_false());
        json_object_set_new(params, "use_secret_chats", json_false());
        json_object_set_new(params, "api_id", json_integer(VITAGRAM_API_ID));
        json_object_set_new(params, "api_hash", json_string(VITAGRAM_API_HASH));
        json_object_set_new(params, "system_language_code", json_string("en"));
        json_object_set_new(params, "device_model", json_string("PlayStation Vita"));
        json_object_set_new(params, "system_version", json_string("PS Vita"));
        json_object_set_new(params, "application_version", json_string(VITAGRAM_VERSION));
        applyProxy();
        sendJson(params);
        status_ = i18n::text("tg.initializing");
    } else if (!std::strcmp(type, "authorizationStateWaitEncryptionKey")) {
        {
            json_t* check = json_pack("{s:s,s:s}", "@type", "checkDatabaseEncryptionKey", "encryption_key",
                                      currentDatabaseKey().c_str());
            sendJson(check);
        }
    } else if (!std::strcmp(type, "authorizationStateWaitPhoneNumber")) {
        auth_detail_.clear(); auth_can_resend_ = false;
        // Re-keying waits for sign-in so the first login steps run exactly as before; only switching
        // the vault off must not wait, or the vault key could never be dropped without a login.
        if (vault::disabling()) migrateDatabaseKey();
        cache_restore_allowed_ = false;
        startup_chat_snapshots_.clear(); startup_list_hold_ = false;
        ready_ = false; known_chats_.clear(); chats_.clear(); leaveChat(); ui_cache_.clear(); user_names_.clear(); supergroup_forum_.clear(); supergroup_member_.clear(); basic_group_member_.clear(); supergroup_rights_.clear(); basic_group_rights_.clear(); reply_target_ = forward_chat_ = forward_message_ = 0; clearSearch(); user_photos_.clear(); sender_requests_.clear();
        folder_windows_.clear(); folders_.clear(); selected_folder_ = 0; syncArchiveFolder(); user_presence_.clear();
        setInput(InputKind::Phone, i18n::text("auth.prompt.phone"));
    } else if (!std::strcmp(type, "authorizationStateWaitCode")) {
        json_t* info = json_object_get(state, "code_info");
        std::string phone = json_str(info, "phone_number");
        while (!phone.empty() && phone[0] == '+') phone.erase(0, 1); // TDLib echoes the number as typed
        auto via_of = [](const char* kind) {
            return !std::strcmp(kind, "authenticationCodeTypeTelegramMessage") ? "auth.via.app" :
                !std::strcmp(kind, "authenticationCodeTypeCall") || !std::strcmp(kind, "authenticationCodeTypeFlashCall") ||
                !std::strcmp(kind, "authenticationCodeTypeMissedCall") ? "auth.via.call" :
                !std::strcmp(kind, "authenticationCodeTypeFragment") ? "auth.via.fragment" : "auth.via.sms";
        };
        auth_detail_ = phone.empty() ? i18n::Text()
            : i18n::text("auth.code_sent", {"+" + phone, i18n::text(via_of(json_str(json_object_get(info, "type"), "@type")))});
        json_t* next = json_object_get(info, "next_type");
        auth_can_resend_ = json_is_object(next);
        auth_next_via_ = auth_can_resend_ ? i18n::text(via_of(json_str(next, "@type"))) : i18n::Text();
        auth_resend_at_ = sceKernelGetSystemTimeWide() + json_i53(info, "timeout") * 1000000;
        setInput(InputKind::Code, i18n::text("auth.prompt.code"));
    } else if (!std::strcmp(type, "authorizationStateWaitEmailAddress")) {
        // Telegram may require a login email for the number instead of sending a code.
        auth_detail_.clear(); auth_can_resend_ = false;
        setInput(InputKind::Email, i18n::text("auth.prompt.email"));
    } else if (!std::strcmp(type, "authorizationStateWaitEmailCode")) {
        json_t* info = json_object_get(state, "code_info");
        const std::string pattern = json_str(info, "email_address_pattern");
        auth_detail_ = pattern.empty() ? i18n::Text() : i18n::text("auth.email_code_sent", {pattern});
        auth_can_resend_ = false;
        setInput(InputKind::EmailCode, i18n::text("auth.prompt.email_code"));
    } else if (!std::strcmp(type, "authorizationStateWaitPremiumPurchase")) {
        // The login requires an in-store Premium purchase, which the console cannot make.
        // TDLib still accepts another phone number or a QR sign-in from this state.
        auth_can_resend_ = false;
        auth_detail_ = i18n::text("auth.premium_hint");
        setInput(InputKind::Phone, i18n::text("auth.premium_required"));
    } else if (!std::strcmp(type, "authorizationStateWaitPassword")) {
        setInput(InputKind::Password, i18n::text("auth.prompt.password"));
    } else if (!std::strcmp(type, "authorizationStateWaitRegistration")) {
        setInput(InputKind::FirstName, i18n::text("auth.prompt.first_name"));
    } else if (!std::strcmp(type, "authorizationStateWaitOtherDeviceConfirmation")) {
        // TDLib refreshes the link about every 30 s; the screen shows the newest one as a QR code.
        qr_link_ = json_str(state, "link");
        input_kind_ = InputKind::None;
        status_ = i18n::text("auth.qr_waiting");
    } else if (!std::strcmp(type, "authorizationStateReady")) {
        migrateDatabaseKey();
        ready_ = true;
        account_user_id_ = 0;
        delete_permissions_.clear();
        sendJson(json_pack("{s:s,s:s}", "@type", "getMe", "@extra", "account_identity"));
        error_.clear();
        status_ = i18n::text("status.online");
        input_kind_ = InputKind::None;
        syncOpenedChat();
        requestChatsPage();
        // Profile photos and thumbnails get per-photo names. Without a file
        // database every re-download collides with older copies: up to 21
        // create attempts, each a directory scan. Counter names need one.
        sendRaw("{\"@type\":\"setOption\",\"name\":\"ignore_file_names\",\"value\":{\"@type\":\"optionValueBoolean\",\"value\":true}}", 10);
        if (current_chat_id_) { history_done_ = false; requestHistoryPage(); }
        next_update_check_ = sceKernelGetSystemTimeWide() + 10000000;
    } else if (!std::strcmp(type, "authorizationStateLoggingOut")) {
        logging_out_ = true;
        if (!account_cleanup::mark()) error_ = i18n::text("account.mark_failed");
        clearAccountMemory();
        status_ = i18n::text("account.logging_out");
    } else if (!std::strcmp(type, "authorizationStateClosed")) {
        closed_ = true;
        ready_ = false;
        opened_chat_id_ = 0;
        connection_ready_ = false;
        status_ = i18n::text("tg.closed");
    } else if (!ready_ && !logging_out_ && std::strcmp(type, "authorizationStateClosing") &&
               !std::strncmp(type, "authorizationState", 18)) {
        // A sign-in step this client does not know must not leave the screen without a prompt:
        // offer the phone number and QR sign-in again; TDLib reports an error if it rejects them.
        auth_detail_.clear(); auth_can_resend_ = false;
        setInput(InputKind::Phone, i18n::text("auth.unknown_step", {std::string(type + 18)}));
    }
}

bool TelegramClient::currentChatIsGroup() const {
    for (const auto& chat : known_chats_) if (chat.id == current_chat_id_) return chat.group;
    return false;
}

TelegramClient::Membership TelegramClient::membershipOf(void* opaque) {
    json_t* status = static_cast<json_t*>(opaque);
    const char* type = json_str(status, "@type");
    const bool is_member = json_is_true(json_object_get(status, "is_member"));
    if (!std::strcmp(type, "chatMemberStatusCreator")) return is_member ? Membership::Creator : Membership::NotMember;
    if (!std::strcmp(type, "chatMemberStatusRestricted")) return is_member ? Membership::Member : Membership::NotMember;
    if (!std::strcmp(type, "chatMemberStatusAdministrator") || !std::strcmp(type, "chatMemberStatusMember")) return Membership::Member;
    if (!std::strcmp(type, "chatMemberStatusLeft") || !std::strcmp(type, "chatMemberStatusBanned")) return Membership::NotMember;
    return Membership::Unknown;
}

TelegramClient::Membership TelegramClient::currentMembership() const {
    for (const auto& chat : known_chats_) if (chat.id == current_chat_id_) {
        if (chat.supergroup_id) {
            auto known = supergroup_member_.find(chat.supergroup_id);
            return known == supergroup_member_.end() ? Membership::Unknown : known->second;
        }
        if (chat.basic_group_id) {
            auto known = basic_group_member_.find(chat.basic_group_id);
            // A basic group cannot be joined again without an invitation.
            if (known == basic_group_member_.end()) return Membership::Unknown;
            return known->second == Membership::NotMember ? Membership::None : known->second;
        }
        break;
    }
    return Membership::None;
}

TelegramClient::PostRights TelegramClient::postRightsOf(void* opaque) {
    json_t* status = static_cast<json_t*>(opaque);
    const char* type = json_str(status, "@type");
    PostRights r;
    if (!std::strcmp(type, "chatMemberStatusCreator")) r.member = r.admin = json_is_true(json_object_get(status, "is_member"));
    else if (!std::strcmp(type, "chatMemberStatusAdministrator")) {
        r.member = true;
        json_t* rights = json_object_get(status, "rights");
        r.admin = json_is_true(json_object_get(rights ? rights : status, "can_post_messages"));
    } else if (!std::strcmp(type, "chatMemberStatusMember")) r.member = true;
    else if (!std::strcmp(type, "chatMemberStatusRestricted"))
        r.member = json_is_true(json_object_get(status, "is_member")) &&
                   json_is_true(json_object_get(json_object_get(status, "permissions"), "can_send_basic_messages"));
    return r;
}

bool TelegramClient::canPostCurrent() const {
    if (!ready_ || !current_chat_id_ || topics_open_) return false;
    for (const auto& chat : known_chats_) if (chat.id == current_chat_id_) {
        if (chat.user_id) return true;
        const auto& rights = chat.supergroup_id ? supergroup_rights_ : basic_group_rights_;
        auto known = rights.find(chat.supergroup_id ? chat.supergroup_id : chat.basic_group_id);
        if (known == rights.end()) return false;
        if (chat.channel) return known->second.admin;
        // Restricted members already carry their own can_send_basic_messages in the rights.
        return known->second.member && (known->second.admin || chat.can_send);
    }
    return false;
}

void TelegramClient::replyTo(int64_t message_id) {
    if (!canPostCurrent() || !message_id) return;
    reply_target_ = message_id;
    requestCompose();
}

// Resolve permissions for every member of a displayed album; cached history carries no rights.
bool TelegramClient::canDeleteMessage(int64_t message_id, bool refresh) {
    if (!ready_ || !current_chat_id_ || message_delete_pending_) return false;
    auto selected = std::find_if(history_items_.begin(), history_items_.end(),
        [&](const MessageItem& item) { return item.id == message_id; });
    if (selected == history_items_.end()) return false;
    bool allowed = true;
    const uint64_t now = sceKernelGetSystemTimeWide();
    for (const auto& item : history_items_) {
        if (item.id != message_id && (!selected->media_album_id || selected->photos.empty() || item.photos.empty() || item.media_album_id != selected->media_album_id)) continue;
        auto& permission = delete_permissions_[item.id];
        if (refresh && (!permission.checked || now - permission.checked > 5000000)) {
            permission.checked = now;
            permission.pending = true;
            auto* request = json_pack("{s:s,s:I,s:I}", "@type", "getMessageProperties",
                "chat_id", (json_int_t)current_chat_id_, "message_id", (json_int_t)item.id);
            json_object_set_new(request, "@extra", json_pack("{s:s,s:I,s:I,s:I}", "kind", "delete_permission",
                "chat", (json_int_t)current_chat_id_, "message", (json_int_t)item.id,
                "generation", (json_int_t)history_generation_));
            sendJson(request);
        }
        allowed = allowed && permission.allowed;
    }
    return allowed;
}

bool TelegramClient::deletePermissionPending(int64_t message_id) const {
    auto selected = std::find_if(history_items_.begin(), history_items_.end(),
        [&](const MessageItem& item) { return item.id == message_id; });
    if (selected == history_items_.end()) return false;
    for (const auto& item : history_items_) {
        if (item.id != message_id && (!selected->media_album_id || selected->photos.empty() || item.photos.empty() || item.media_album_id != selected->media_album_id)) continue;
        auto permission = delete_permissions_.find(item.id);
        if (permission != delete_permissions_.end() && permission->second.pending) return true;
    }
    return false;
}

void TelegramClient::deleteMessage(int64_t message_id) {
    if (!canDeleteMessage(message_id, false)) {
        notice_ = i18n::text("message.delete_failed", {i18n::text("message.delete_rights")});
        notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
        return;
    }
    const auto selected = std::find_if(history_items_.begin(), history_items_.end(),
        [&](const MessageItem& item) { return item.id == message_id; });
    json_t* ids = json_array();
    bool revoke = true;
    for (const auto& item : history_items_)
        if (item.id == message_id || (selected->media_album_id && !selected->photos.empty() && !item.photos.empty() && item.media_album_id == selected->media_album_id)) {
            json_array_append_new(ids, json_integer(item.id));
            revoke = revoke && delete_permissions_.at(item.id).revoke;
        }
    auto* request = json_pack("{s:s,s:I,s:O,s:b}", "@type", "deleteMessages",
        "chat_id", (json_int_t)current_chat_id_, "message_ids", ids, "revoke", revoke);
    json_object_set_new(request, "@extra", json_pack("{s:s,s:I,s:o}", "kind", "delete_message",
        "chat", (json_int_t)current_chat_id_, "ids", ids));
    message_delete_pending_ = true;
    message_delete_deadline_ = sceKernelGetSystemTimeWide() + 30000000;
    notice_ = i18n::text("message.deleting"); notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
    sendJson(request);
}

void TelegramClient::beginForward(int64_t message_id) {
    if (!current_chat_id_ || !message_id) return;
    forward_chat_ = current_chat_id_; forward_message_ = message_id;
    notice_ = i18n::text("notice.forward_picked");
    notice_until_ = sceKernelGetSystemTimeWide() + 4000000;
}

bool TelegramClient::pasteForward() {
    if (!forward_message_ || !canPostCurrent()) return false;
    json_t* ids = json_array();
    json_array_append_new(ids, json_integer(forward_message_));
    auto* request = json_pack("{s:s,s:I,s:I,s:o,s:b,s:b}", "@type", "forwardMessages", "chat_id", (json_int_t)current_chat_id_,
                              "from_chat_id", (json_int_t)forward_chat_, "message_ids", ids, "send_copy", 1, "remove_caption", 0);
    if (current_topic_)
        json_object_set_new(request, "topic_id", json_pack("{s:s,s:i}", "@type", "messageTopicForum", "forum_topic_id", current_topic_));
    json_object_set_new(request, "@extra", json_string("send_message"));
    sendJson(request);
    forward_message_ = forward_chat_ = 0;
    status_ = i18n::text("common.sending");
    return true;
}

void TelegramClient::joinCurrentChat() {
    if (!ready_ || !current_chat_id_ || membership_pending_) return;
    membership_pending_ = true;
    auto* request = json_pack("{s:s,s:I}", "@type", "joinChat", "chat_id", (json_int_t)current_chat_id_);
    json_object_set_new(request, "@extra", json_pack("{s:s,s:s,s:I}", "kind", "group_action", "action", "join", "chat", (json_int_t)current_chat_id_));
    sendJson(request);
}

bool TelegramClient::currentChatIsPrivate() const {
    for (const auto& chat : known_chats_) if (chat.id == current_chat_id_) return chat.user_id != 0;
    return false;
}

void TelegramClient::deleteCurrentHistory(bool remove) {
    if (!ready_ || !current_chat_id_ || membership_pending_ || !currentChatIsPrivate()) return;
    membership_pending_ = true;
    auto* request = json_pack("{s:s,s:I,s:b,s:b}", "@type", "deleteChatHistory", "chat_id", (json_int_t)current_chat_id_,
                              "remove_from_chat_list", remove, "revoke", 0);
    json_object_set_new(request, "@extra", json_pack("{s:s,s:s,s:I}", "kind", "history_action", "action", remove ? "delete" : "clear", "chat", (json_int_t)current_chat_id_));
    sendJson(request);
}

void TelegramClient::leaveCurrentGroup() {
    if (!ready_ || !current_chat_id_ || membership_pending_) return;
    membership_pending_ = true;
    auto* request = json_pack("{s:s,s:I}", "@type", "leaveChat", "chat_id", (json_int_t)current_chat_id_);
    json_object_set_new(request, "@extra", json_pack("{s:s,s:s,s:I}", "kind", "group_action", "action", "leave", "chat", (json_int_t)current_chat_id_));
    sendJson(request);
}

void TelegramClient::searchChats(const std::string& query) {
    search_query_ = query;
    ++search_generation_;
    for (auto& ids : search_ids_) ids.clear();
    search_pending_ = 0;
    if (!ready_ || query.empty()) return;
    static const char* const methods[3] = {"searchChats", "searchChatsOnServer", "searchPublicChats"};
    for (int source = 0; source < 3; ++source) {
        auto* request = json_pack("{s:s,s:s}", "@type", methods[source], "query", query.c_str());
        if (source < 2) json_object_set_new(request, "limit", json_integer(20));
        json_object_set_new(request, "@extra", json_pack("{s:s,s:i,s:i}", "kind", "search_chats", "generation", (int)search_generation_, "source", source));
        sendJson(request);
        ++search_pending_;
    }
}

void TelegramClient::clearSearch() {
    ++search_generation_;
    search_pending_ = 0;
    search_query_.clear();
    for (auto& ids : search_ids_) ids.clear();
}

std::vector<ChatItem> TelegramClient::searchResults() const {
    std::vector<ChatItem> out;
    for (const auto& ids : search_ids_) for (int64_t id : ids) {
        if (std::any_of(out.begin(), out.end(), [&](const ChatItem& c) { return c.id == id; })) continue;
        auto found = std::find_if(known_chats_.begin(), known_chats_.end(), [&](const ChatItem& c) { return c.id == id; });
        if (found != known_chats_.end()) out.push_back(*found);
    }
    return out;
}

void TelegramClient::ensureSearchMedia(int64_t chat_id) {
    for (const auto& chat : known_chats_) if (chat.id == chat_id) {
        if (chat.avatar_path.empty() && chat.avatar_file_id) requestFile(chat.avatar_file_id, 8, true);
        return;
    }
}

void TelegramClient::parseUser(void* opaque) {
    json_t* user = static_cast<json_t*>(opaque);
    int64_t id = json_i53(user, "id");
    if (!id) return;
    std::string name = user_identity::name(user);
    const bool deleted = user_identity::deleted(user);
    user_names_[id] = deleted ? "" : name;
    json_t* photo = json_object_get(json_object_get(user, "profile_photo"), "small");
    json_t* local = json_object_get(photo, "local");
    auto& avatar = user_photos_[id];
    avatar.deleted = deleted;
    avatar.file = (int32_t)json_i53(photo, "id");
    avatar.path = json_is_true(json_object_get(local, "is_downloading_completed")) ? json_str(local, "path") : "";
    avatar.fresh = true;
    if (deleted) { avatar.file = 0; avatar.path.clear(); }
    if (json_is_object(json_object_get(user, "status"))) parseUserStatus(id, json_object_get(user, "status"));
    if (json_is_object(json_object_get(user, "type")))
        user_presence_[id].bot = !std::strcmp(json_str(json_object_get(user, "type"), "@type"), "userTypeBot");
}

void TelegramClient::parseUserStatus(int64_t id, void* opaque) {
    json_t* status = static_cast<json_t*>(opaque);
    auto& presence = user_presence_[id];
    presence.type = json_str(status, "@type");
    presence.time = json_i53(status, presence.type == "userStatusOnline" ? "expires" : "was_online");
}

std::string TelegramClient::currentUserStatus() const {
    int64_t id = 0;
    for (const auto& chat : known_chats_) if (chat.id == current_chat_id_) { id = chat.user_id; break; }
    if (!id) return "";
    auto found = user_presence_.find(id);
    if (found == user_presence_.end()) return tr("presence.loading");
    const auto& p = found->second;
    if (p.bot) return tr("presence.bot");
    if (p.type == "userStatusOnline") return ready_ && p.time > (int64_t)std::time(nullptr) ? tr("presence.online") : tr("presence.refining");
    if (p.type == "userStatusRecently") return tr("presence.recently");
    if (p.type == "userStatusLastWeek") return tr("presence.last_week");
    if (p.type == "userStatusLastMonth") return tr("presence.last_month");
    if (p.type == "userStatusOffline" && p.time > 0) {
        SceDateTime date{}; SceRtcTick utc{}, local{};
        if (sceRtcSetTime_t(&date, (time_t)p.time) >= 0 && sceRtcGetTick(&date, &utc) >= 0) {
            bool local_ok = sceRtcConvertUtcToLocalTime(&utc, &local) >= 0 && sceRtcSetTick(&date, &local) >= 0;
            char text[96];
            std::snprintf(text, sizeof(text), "%02u.%02u.%04u %02u:%02u%s", (unsigned)date.day,
                (unsigned)date.month, (unsigned)date.year, (unsigned)date.hour, (unsigned)date.minute, local_ok ? "" : " UTC");
            return tr("presence.last_seen_at", {text});
        }
    }
    return tr("presence.unavailable");
}

i18n::Text TelegramClient::senderName(const MessageItem& message) const {
    if (message.sender_chat) {
        for (const auto& chat : known_chats_) if (chat.id == message.sender_id) return chat.title;
    } else {
        auto it = user_names_.find(message.sender_id);
        if (it != user_names_.end()) return senderIsDeleted(message) ? i18n::text("user.deleted") : it->second.empty() ? i18n::text("user.participant") : i18n::Text(it->second);
    }
    return i18n::text("user.participant");
}

std::string TelegramClient::senderAvatar(const MessageItem& message) const {
    if (message.sender_chat) {
        for (const auto& chat : known_chats_) if (chat.id == message.sender_id) return chat.avatar_path;
    } else {
        auto it = user_photos_.find(message.sender_id);
        if (it != user_photos_.end()) return it->second.path;
    }
    return "";
}

bool TelegramClient::senderIsDeleted(const MessageItem& message) const {
    if (message.sender_chat) return false;
    auto it = user_photos_.find(message.sender_id);
    return it != user_photos_.end() && it->second.deleted;
}

bool TelegramClient::chatIsDeleted(const ChatItem& chat) const {
    if (!chat.user_id) return false;
    MessageItem sender; sender.sender_id = chat.user_id;
    return senderIsDeleted(sender);
}

i18n::Text TelegramClient::chatTitle(const ChatItem& chat) const {
    return chatIsDeleted(chat) ? i18n::text("user.deleted") : i18n::Text(chat.title);
}

bool TelegramClient::currentChatIsDeleted() const {
    for (const auto& chat : known_chats_) if (chat.id == current_chat_id_) return chatIsDeleted(chat);
    return false;
}

i18n::Text TelegramClient::currentChatTitle() const {
    return currentChatIsDeleted() ? i18n::text("user.deleted") : current_chat_title_;
}

i18n::Text TelegramClient::serviceMessageText(const MessageItem& message) const {
    if (message.pin_message) return i18n::text("service.message_pinned");
    if (message.member_removed) {
        MessageItem member;
        member.sender_id = message.removed_member_id;
        i18n::Text name = senderName(member);
        if (!message.sender_chat && message.sender_id == message.removed_member_id)
            return i18n::text("service.member_left", {name});
        return i18n::text("service.member_removed", {senderName(message), name});
    }
    return i18n::text("service.joined_by_link", {senderName(message)});
}

void TelegramClient::ensureSenderMedia(const MessageItem& message) {
    performance::SlowScope slow(performance::SlowPhase::Ui,"ensure_sender_media");
    if (!ready_ || !message.sender_id || !currentChatIsGroup() || message.outgoing) return;
    bool fresh = false; int32_t file = 0; std::string path;
    if (message.sender_chat) {
        for (const auto& chat : known_chats_) if (chat.id == message.sender_id) {
            fresh = !chat.cached_metadata; file = chat.avatar_file_id; path = chat.avatar_path; break;
        }
    } else {
        auto it = user_photos_.find(message.sender_id);
        if (it != user_photos_.end()) { fresh = it->second.fresh; file = it->second.file; path = it->second.path; }
    }
    if (!fresh) {
        std::string key = std::string(message.sender_chat ? "c" : "u") + std::to_string(message.sender_id);
        int64_t now = sceKernelGetSystemTimeWide();
        if (now >= sender_requests_[key]) {
            sender_requests_[key] = now + 10000000;
            sendJson(json_pack("{s:s,s:I,s:s}", "@type", message.sender_chat ? "getChat" : "getUser",
                message.sender_chat ? "chat_id" : "user_id", (json_int_t)message.sender_id, "@extra", "sender_lookup"));
        }
    }
    if (path.empty()) requestFile(file, 8);
}

ChatItem TelegramClient::chatFromJson(void* opaque) const {
    json_t* chat = (json_t*)opaque;
    ChatItem item;
    json_t* chat_type = json_object_get(chat, "type");
    item.supergroup_id = json_i53(chat_type, "supergroup_id");
    item.basic_group_id = json_i53(chat_type, "basic_group_id");
    item.channel = !std::strcmp(json_str(chat_type, "@type"), "chatTypeSupergroup") && json_is_true(json_object_get(chat_type, "is_channel"));
    if (!std::strcmp(json_str(chat_type, "@type"), "chatTypePrivate") || !std::strcmp(json_str(chat_type, "@type"), "chatTypeSecret"))
        item.user_id = json_i53(chat_type, "user_id");
    item.group = !std::strcmp(json_str(chat_type, "@type"), "chatTypeBasicGroup") ||
        (!std::strcmp(json_str(chat_type, "@type"), "chatTypeSupergroup") && !json_is_true(json_object_get(chat_type, "is_channel")));
    item.id = json_i53(chat, "id");
    if (!item.id) return item;
    if (json_t* permissions = json_object_get(chat, "permissions"))
        item.can_send = json_is_true(json_object_get(permissions, "can_send_basic_messages"));

    // Retain metadata across tabs; membership/order belongs to each TDLib list.
    json_t* positions = json_object_get(chat, "positions");
    if (json_is_array(positions)) {
        size_t position_index; json_t* position;
        json_array_foreach(positions, position_index, position) {
            json_t* list = json_is_object(position) ? json_object_get(position, "list") : nullptr;
            int32_t list_id = chat_list_id(list);
            if (list_id != -1 && json_i53(position, "order")) item.list_orders[list_id] = json_i53(position, "order");
        }
    }
    item.title = json_str(chat, "title", "(untitled)");
    item.unread_count = (int)json_i53(chat, "unread_count");
    item.marked_unread = json_is_true(json_object_get(chat, "is_marked_as_unread"));
    item.last_message_id = json_i53(json_object_get(chat, "last_message"), "id");
    json_t* photo = json_object_get(chat, "photo");
    json_t* small = json_is_object(photo) ? json_object_get(photo, "small") : nullptr;
    if (json_is_object(small)) {
        item.avatar_file_id = (int32_t)json_i53(small, "id");
        json_t* local = json_object_get(small, "local");
        if (json_is_object(local) && json_is_true(json_object_get(local, "is_downloading_completed"))) item.avatar_path = json_str(local, "path");
    }
    return item;
}
void TelegramClient::parseChat(void* opaque) {
    json_t* chat = (json_t*)opaque;
    ChatItem item = chatFromJson(chat);
    if (!item.id) return;
    updateReadOutbox(item.id, json_i53(chat, "last_read_outbox_message_id"));
    auto it = std::find_if(known_chats_.begin(), known_chats_.end(), [&](const ChatItem& c){ return c.id == item.id; });
    if (it != known_chats_.end()) {
        noteLastMessage(item.id, json_object_get(chat, "last_message"));
        item.last_message_id = std::max(item.last_message_id, it->last_message_id);
        // Keep the picture on screen until TDLib's download replaces it (updateFile); a
        // removed photo (no file) still clears it.
        if (item.avatar_path.empty() && item.avatar_file_id && !it->avatar_path.empty()) {
            item.avatar_path = it->avatar_path; item.avatar_stale = true;
        }
    }
    if (it == known_chats_.end()) known_chats_.push_back(item);
    else *it = item;
}


static void parse_reactions(MessageItem& message, json_t* info) {
    message.reactions.clear();
    auto* nested=json_object_get(info,"interaction_info");
    if (nested) info=nested;
    auto* reactions=json_object_get(info,"reactions");
    if (json_is_object(reactions)) reactions=json_object_get(reactions,"reactions");
    size_t i;json_t* reaction;
    json_array_foreach(reactions,i,reaction) {
        auto* type=json_object_get(reaction,"type");
        const char* kind=json_str(type,"@type");
        const auto custom=json_i53(type,"custom_emoji_id");
        std::string emoji=!std::strcmp(kind,"reactionTypeEmoji") ? json_str(type,"emoji") :
            !std::strcmp(kind,"reactionTypePaid") ? "Stars" : "Custom";
        if (emoji.empty()) continue;
        message.reactions.push_back({emoji,(int)json_i53(reaction,"total_count"),json_is_true(json_object_get(reaction,"is_chosen")),custom});
    }
}

// Comments exist only for channel posts that have a discussion: TDLib then sends reply_info (null otherwise),
// so nothing is invented for posts without one. `info` is a messageInteractionInfo or an update carrying it.
static void parse_comments(MessageItem& message, json_t* info) {
    auto* nested=json_object_get(info,"interaction_info");
    if (nested) info=nested;
    auto* reply=json_object_get(info,"reply_info");
    message.has_comments=message.channel_post && json_is_object(reply);
    message.comment_count=message.has_comments ? std::max(0,(int)json_i53(reply,"reply_count")) : 0;
    message.comments_message_id=message.has_comments ? message.id : 0;
}

// voiceNote.waveform is base64 of 5-bit samples packed low bits first (Telegram's format).
static std::vector<uint8_t> decode_waveform(const char* encoded) {
    std::vector<uint8_t> bytes, samples;
    if (!encoded || !*encoded) return samples;
    uint32_t accumulator = 0; int bits = 0;
    for (const char* p = encoded; *p && *p != '='; ++p) {
        int v;
        if (*p >= 'A' && *p <= 'Z') v = *p - 'A'; else if (*p >= 'a' && *p <= 'z') v = *p - 'a' + 26;
        else if (*p >= '0' && *p <= '9') v = *p - '0' + 52; else if (*p == '+' || *p == '-') v = 62;
        else if (*p == '/' || *p == '_') v = 63; else continue;
        accumulator = (accumulator << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; bytes.push_back((uint8_t)((accumulator >> bits) & 255)); }
    }
    const size_t count = std::min<size_t>(bytes.size() * 8 / 5, 256);
    for (size_t i = 0; i < count; ++i) {
        const size_t byte = i * 5 / 8, shift = i * 5 % 8;
        const uint32_t pair = bytes[byte] | (byte + 1 < bytes.size() ? (uint32_t)bytes[byte + 1] << 8 : 0u);
        samples.push_back((uint8_t)((pair >> shift) & 31));
    }
    return samples;
}

static void parse_transcription(MessageItem& message, json_t* note) {
    json_t* result = json_object_get(note, "speech_recognition_result");
    const char* type = json_str(result, "@type");
    message.transcription_pending = !std::strcmp(type, "speechRecognitionResultPending");
    message.transcription_complete = !std::strcmp(type, "speechRecognitionResultText");
    message.transcription_error = !std::strcmp(type, "speechRecognitionResultError") ?
        json_str(json_object_get(result, "error"), "message") : "";
    message.transcription = !std::strcmp(type, "speechRecognitionResultText") ? json_str(result, "text") :
        message.transcription_pending ? json_str(result, "partial_text") : "";
    if (message.transcription.find_first_not_of(" \t\r\n") == std::string::npos) message.transcription.clear();
}


// One-line preview of a message for reply quotes: its text, otherwise its media label.
static i18n::Text reply_preview_text(const MessageItem& item) {
    return i18n::Text::computed([source = item.text.empty() ? item.media_label : item.text] {
        std::string text = source.localized();
        for (char& c : text) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (text.size() > 200) {
            size_t end = 200;
            while (end && ((unsigned char)text[end] & 0xc0) == 0x80) --end;
            text.resize(end);
        }
        return text;
    });
}

MessageItem TelegramClient::parseMessage(void* opaque) {
    json_t* msg = (json_t*)opaque;
    MessageItem out;
    parse_reactions(out, json_object_get(msg, "interaction_info"));
    json_t* sender = json_object_get(msg, "sender_id");
    out.sender_chat = !std::strcmp(json_str(sender, "@type"), "messageSenderChat");
    out.sender_id = json_i53(sender, out.sender_chat ? "chat_id" : "user_id");
    out.id = json_i53(msg, "id");
    out.channel_post = json_is_true(json_object_get(msg, "is_channel_post"));
    parse_comments(out, json_object_get(msg, "interaction_info"));
    out.date = json_i53(msg, "date");
    out.edit_date = json_i53(msg, "edit_date");
    out.media_album_id = json_i53(msg, "media_album_id");
    out.outgoing = json_is_true(json_object_get(msg, "is_outgoing"));
    json_t* sending = json_object_get(msg, "sending_state");
    const char* sending_type = json_str(sending, "@type");
    if (!std::strcmp(sending_type, "messageSendingStatePending")) out.delivery = DeliveryState::Pending;
    else if (!std::strcmp(sending_type, "messageSendingStateFailed")) {
        out.delivery = DeliveryState::Failed;
        out.send_error = json_str(json_object_get(sending, "error"), "message", "Message could not be sent");
    } else if (out.outgoing && out.id > 0 && out.id <= read_outbox_[json_i53(msg, "chat_id")]) out.delivery = DeliveryState::Read;
    json_t* content = json_object_get(msg, "content");
    const char* type = json_is_object(content) ? json_str(content, "@type") : "";
    if (!std::strcmp(type, "messagePoll")) {
        out.poll = PollData::parse(json_object_get(content, "poll"));
        out.media_label = out.poll.quiz ? i18n::text("media.quiz") : i18n::text("media.poll");
        out.text = out.poll.question;
    } else if (!std::strcmp(type, "messageRichMessage")) {
        auto* blocks = json_object_get(json_object_get(content, "message"), "blocks");
        rich_message::blocks(out.text, blocks);
        auto visit = [&](json_t* block) {
            const char* block_type = json_str(block, "@type");
            const char* media_type = nullptr;
            const char* media_key = nullptr;
            if (!std::strcmp(block_type, "pageBlockPhoto")) { media_type = "messagePhoto"; media_key = "photo"; }
            else if (!std::strcmp(block_type, "pageBlockVideo")) { media_type = "messageVideo"; media_key = "video"; }
            else if (!std::strcmp(block_type, "pageBlockAnimation")) { media_type = "messageAnimation"; media_key = "animation"; }
            else if (!std::strcmp(block_type, "pageBlockAudio")) { media_type = "messageAudio"; media_key = "audio"; }
            else if (!std::strcmp(block_type, "pageBlockVoiceNote")) { media_type = "messageVoiceNote"; media_key = "voice_note"; }
            else if (!std::strcmp(block_type, "pageBlockDocument")) { media_type = "messageDocument"; media_key = "document"; }
            if (!media_type) return;
            i18n::Text label = !std::strcmp(media_type, "messagePhoto") ? i18n::text("media.photo") :
                !std::strcmp(media_type, "messageVideo") ? i18n::text("media.video") :
                !std::strcmp(media_type, "messageAnimation") ? i18n::Text("GIF") :
                !std::strcmp(media_type, "messageAudio") ? i18n::text("media.audio") :
                !std::strcmp(media_type, "messageVoiceNote") ? i18n::text("media.voice") : i18n::text("media.file");
            if (out.media_label.find(label) == std::string::npos) {
                if (!out.media_label.empty()) out.media_label += " · ";
                out.media_label += label;
            }
            auto* media = json_object_get(block, media_key);
            if (!json_is_object(media)) return;
            auto* nested_content = json_pack("{s:s}", "@type", media_type);
            json_object_set(nested_content, media_key, media);
            auto* nested_message = json_pack("{s:o}", "content", nested_content);
            auto parsed = parseMessage(nested_message);
            json_decref(nested_message);
            out.photos.insert(out.photos.end(), parsed.photos.begin(), parsed.photos.end());
            if (parsed.animation && !out.animation) {
                out.animation = true; out.video = parsed.video;
                out.video_duration = parsed.video_duration;
                out.video_width = parsed.video_width; out.video_height = parsed.video_height;
                out.video_alternative = parsed.video_alternative;
                out.animation_file_id = parsed.animation_file_id;
                out.animation_size = parsed.animation_size; out.animation_downloaded = parsed.animation_downloaded;
                out.animation_path = std::move(parsed.animation_path); out.animation_error = std::move(parsed.animation_error);
                out.animation_stream = std::move(parsed.animation_stream);
                out.media_file_id = parsed.media_file_id; out.media_path = std::move(parsed.media_path);
                out.media_fallback = std::move(parsed.media_fallback);
                out.media_video_preview = parsed.media_video_preview;
            }
            if (parsed.voice && !out.voice) {
                out.voice = true; out.audio = parsed.audio;
                out.audio_title = std::move(parsed.audio_title); out.audio_performer = std::move(parsed.audio_performer);
                out.voice_duration = parsed.voice_duration; out.voice_file_id = parsed.voice_file_id;
                out.voice_size = parsed.voice_size; out.voice_downloaded = parsed.voice_downloaded;
                out.voice_path = std::move(parsed.voice_path); out.voice_error = std::move(parsed.voice_error);
                out.voice_stream = std::move(parsed.voice_stream);
            }
            if (parsed.document && !out.document) {
                out.document = true; out.document_name = std::move(parsed.document_name);
                out.document_size = parsed.document_size; out.document_downloaded = parsed.document_downloaded;
                if (!out.media_file_id) { out.media_file_id = parsed.media_file_id; out.media_path = std::move(parsed.media_path); }
            }
        };
        rich_message::visit_blocks(blocks, visit);
        if (out.photos.size() > 1) {
            const i18n::Text photo_label = i18n::text("media.photo");
            auto position = out.media_label.find(photo_label);
            if (position != std::string::npos)
                out.media_label.replaceTranslation(photo_label, i18n::text("media.photos_count", {std::to_string(out.photos.size())}));
        }
    } else if (!std::strcmp(type, "messageText")) {
        json_t* ft = json_object_get(content, "text");
        out.text = json_is_object(ft) ? json_str(ft, "text", "") : "";
    } else if (!std::strcmp(type, "messageAnimation") || !std::strcmp(type, "messageVideo") || !std::strcmp(type, "messageVideoNote")) {
        out.animation = true;
        out.spoiler = json_is_true(json_object_get(content, "has_spoiler"));
        out.video = std::strcmp(type, "messageAnimation") != 0;
        out.video_note = !std::strcmp(type, "messageVideoNote");
        out.media_label = out.video_note ? i18n::text("media.video_note") : out.video ? i18n::text("media.video") : "GIF";
        out.text = json_str(json_object_get(content, "caption"), "text");
        json_t* animation = json_object_get(content, out.video_note ? "video_note" : out.video ? "video" : "animation");
        out.media_fallback = inline_thumbnail::key(json_str(json_object_get(animation, "minithumbnail"), "data"));
        if (out.video_note) parse_transcription(out, animation);
        out.video_duration = std::max<int64_t>(0, json_i53(animation, "duration"));
        json_t* file = json_object_get(animation, out.video ? "video" : "animation");
        int64_t w = json_i53(animation, "width"), h = json_i53(animation, "height");
        if (out.video_note) w = h = json_i53(animation, "length");
        if (out.video && !out.video_note) {
            auto quality = VideoQuality::minimum(content);
            file = quality.file; w = quality.width; h = quality.height;
            out.video_alternative = quality.alternative;
        }
        out.video_width = (int)w; out.video_height = (int)h;
        out.animation_file_id = (int32_t)json_i53(file, "id");
        out.animation_size = std::max(json_i53(file, "size"), json_i53(file, "expected_size"));
        json_t* local = json_object_get(file, "local");
        out.animation_downloaded = json_i53(local, "downloaded_size");
        if (out.video) out.animation_stream = mediaStream(file);
        if (json_is_true(json_object_get(local, "is_downloading_completed"))) out.animation_path = json_str(local, "path");

        if (out.animation_size > (out.video ? 128 : 20) * 1024 * 1024) out.animation_error = out.video ? i18n::text("anim.limit_video") : i18n::text("anim.limit_gif");
        else if (w > 1280 || h > 1280 || (w > 0 && h > 0 && w * h > 1280 * 720))
            out.animation_error = i18n::text("anim.limit_720p_media");
        else if (json_is_true(json_object_get(content, "is_secret"))) out.animation_error = i18n::text("anim.self_destructing");
        json_t* thumb = json_object_get(animation, "thumbnail");
        const char* format = json_str(json_object_get(thumb, "format"), "@type");
        out.media_video_preview = !out.video && GifCatalog::videoPreview(animation);
        if (!std::strcmp(format, "thumbnailFormatJpeg") || !std::strcmp(format, "thumbnailFormatPng") || !std::strcmp(format, "thumbnailFormatWebp") || out.media_video_preview) {
            json_t* preview = json_object_get(thumb, "file");
            out.media_file_id = (int32_t)json_i53(preview, "id");
            json_t* preview_local = json_object_get(preview, "local");
            if (json_is_true(json_object_get(preview_local, "is_downloading_completed"))) out.media_path = GifCatalog::previewPath(out.media_video_preview, json_str(preview_local, "path"));
        }
    } else if (!std::strcmp(type, "messageAnimatedEmoji")) {
        // Render the Unicode fallback without downloading animation or sound.
        out.text = json_str(content, "emoji");
    } else if (!std::strcmp(type, "messagePhoto")) {
        json_t* caption = json_object_get(content, "caption");
        const char* caption_text = json_is_object(caption) ? json_str(caption, "text") : "";
        out.media_label = i18n::text("media.photo");
        out.text = caption_text;
        out.spoiler = json_is_true(json_object_get(content, "has_spoiler"));
        json_t* photo = json_object_get(content, "photo");
        json_t* sizes = json_is_object(photo) ? json_object_get(photo, "sizes") : nullptr;
        if (json_is_array(sizes) && json_array_size(sizes) != 0) {
            // Pick an on-screen-sized variant instead of downloading the full photo.
            // TDLib returns sizes from small to large; ~320 px is ample for this UI.
            json_t* size = json_array_get(sizes, json_array_size(sizes) - 1);
            size_t index; json_t* candidate;
            json_array_foreach(sizes, index, candidate) {
                size = candidate;
                if (json_i53(candidate, "width") >= 320 || json_i53(candidate, "height") >= 240) break;
            }
            json_t* file = json_is_object(size) ? json_object_get(size, "photo") : nullptr;
            if (json_is_object(file)) {
                out.media_file_id = (int32_t)json_i53(file, "id");
                json_t* local = json_object_get(file, "local");
                if (json_is_object(local) && json_is_true(json_object_get(local, "is_downloading_completed"))) out.media_path = json_str(local, "path");
            }
            json_t* full_size = json_array_get(sizes, json_array_size(sizes) - 1);
            // Use the largest available screen-sized photo for Vita's limited RAM.
            int64_t best_area = 0;
            json_array_foreach(sizes, index, candidate) {
                int64_t width = json_i53(candidate, "width"), height = json_i53(candidate, "height");
                if (width > 0 && height > 0 && width <= 1280 && height <= 1280 && width * height > best_area) {
                    best_area = width * height;
                    full_size = candidate;
                }
            }
            if (!best_area) full_size = size;
            json_t* full_file = json_is_object(full_size) ? json_object_get(full_size, "photo") : nullptr;
            PhotoItem photo_item;
            photo_item.width = (int)json_i53(full_size, "width");
            photo_item.height = (int)json_i53(full_size, "height");
            photo_item.thumbnail_file_id = out.media_file_id;
            photo_item.thumbnail_path = out.media_path;
            if (json_is_object(full_file)) {
                photo_item.full_file_id = (int32_t)json_i53(full_file, "id");
                json_t* full_local = json_object_get(full_file, "local");
                if (json_is_object(full_local) && json_is_true(json_object_get(full_local, "is_downloading_completed"))) photo_item.full_path = json_str(full_local, "path");
            }
            out.photos.push_back(photo_item);
        }
    } else if (!std::strcmp(type, "messageSticker")) {
        out.sticker = true;
        json_t* sticker = json_object_get(content, "sticker");
        bool animated = std::strcmp(json_str(json_object_get(sticker, "format"), "@type"), "stickerFormatWebp") != 0;
        if (animated) {
            out.animation = true;
            json_t* animation = json_object_get(sticker, "sticker");
            out.animation_file_id = (int32_t)json_i53(animation, "id");
            out.animation_size = json_i53(animation, "size");
            json_t* local = json_object_get(animation, "local");
            if (json_is_true(json_object_get(local, "is_downloading_completed"))) out.animation_path = json_str(local, "path");
        }
        out.media_label = animated ? i18n::text("media.sticker_animated") : i18n::text("media.sticker");
        out.text = json_str(sticker, "emoji");
        json_t* thumb = json_is_object(sticker) ? json_object_get(sticker, "thumbnail") : nullptr;
        json_t* file = animated ? json_object_get(thumb, "file") : json_object_get(sticker, "sticker");
        if (animated) {
            const char* format = json_str(json_object_get(thumb, "format"), "@type");
            if (std::strcmp(format, "thumbnailFormatWebp") && std::strcmp(format, "thumbnailFormatJpeg") &&
                std::strcmp(format, "thumbnailFormatPng")) file = nullptr;
        }
        if (json_is_object(file)) {
            out.media_file_id = (int32_t)json_i53(file, "id");
            json_t* local = json_object_get(file, "local");
            if (json_is_object(local) && json_is_true(json_object_get(local, "is_downloading_completed"))) out.media_path = json_str(local, "path");
        }
    }
    else if (!std::strcmp(type, "messageVoiceNote") || !std::strcmp(type, "messageAudio")) {
        out.voice = true;
        out.audio = !std::strcmp(type, "messageAudio");
        json_t* note = json_object_get(content, out.audio ? "audio" : "voice_note");
        if (!out.audio) { parse_transcription(out, note); out.waveform = decode_waveform(json_str(note, "waveform")); }
        json_t* file = json_object_get(note, out.audio ? "audio" : "voice");
        if (out.audio) {
            out.audio_title = json_str(note, "title");
            if (out.audio_title.empty()) out.audio_title = json_str(note, "file_name");
            if (out.audio_title.empty()) out.audio_title = i18n::text("media.audio");
            out.audio_performer = json_str(note, "performer");
        }
        json_t* local = json_object_get(file, "local");
        out.voice_duration = std::max<int64_t>(0, json_i53(note, "duration"));
        out.voice_file_id = (int32_t)json_i53(file, "id");
        out.voice_size = std::max(json_i53(file, "size"), json_i53(file, "expected_size"));
        out.voice_downloaded = json_i53(local, "downloaded_size");
        out.voice_stream = mediaStream(file);
        const char* local_path = json_is_object(local) ? json_str(local, "path") : "";
        if (json_is_true(json_object_get(local, "is_downloading_completed")) && local_path && *local_path) {
            std::string normalized(local_path);
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            bool audio_like = normalized.find(".ogg") != std::string::npos || normalized.find(".oga") != std::string::npos ||
                              normalized.find(".opus") != std::string::npos || normalized.find(".m4a") != std::string::npos ||
                              normalized.find(".mp3") != std::string::npos || normalized.find(".wav") != std::string::npos ||
                              normalized.find("/voice-") != std::string::npos || normalized.find("/audio-") != std::string::npos;
            if (audio_like) out.voice_path = local_path;
            else out.voice_path.clear();
        } else {
            out.voice_path.clear();
        }
        if (out.voice_size > (out.audio ? 128 : 20) * 1024 * 1024) out.voice_error = out.audio ? i18n::text("voice.limit_audio") : i18n::text("voice.limit_voice");
        out.media_label = out.audio ? i18n::text("media.audio") : i18n::text("media.voice");
        out.text = out.audio ? out.audio_title + (out.audio_performer.empty() ? "" : " - " + out.audio_performer) : "";
        std::string caption = json_str(json_object_get(content, "caption"), "text");
        if (!caption.empty()) out.text += "\n" + caption;
    }
    else if (!std::strcmp(type, "messageDocument")) {
        out.document = true;
        json_t* document = json_object_get(content, "document");
        json_t* file = json_object_get(document, "document");
        json_t* local = json_object_get(file, "local");
        out.document_name = json_str(document, "file_name");
        out.media_file_id = (int32_t)json_i53(file, "id");
        out.document_size = std::max(json_i53(file, "size"), json_i53(file, "expected_size"));
        out.document_downloaded = json_i53(local, "downloaded_size");
        if (json_is_true(json_object_get(local, "is_downloading_completed"))) out.media_path = json_str(local, "path");
        out.media_label = i18n::text("media.file");
        out.text = out.document_name.empty() ? i18n::text("media.no_name") : i18n::Text(out.document_name);
        std::string caption = json_str(json_object_get(content, "caption"), "text");
        if (!caption.empty()) out.text += "\n" + caption;
    }
    else if (!std::strcmp(type, "messagePinMessage")) {
        out.pin_message = true;
        out.service_message = true;
        out.pinned_message_id = json_i53(content, "message_id");
        out.text = i18n::text("service.message_pinned");
    }
    else if (!std::strcmp(type, "messageChatJoinByLink")) {
        out.service_message = true;
        out.text = i18n::text("service.joined_by_link", {senderName(out)});
    }
    else if (!std::strcmp(type, "messageChatDeleteMember")) {
        out.service_message = true;
        out.member_removed = true;
        out.removed_member_id = json_i53(content, "user_id");
        out.text = serviceMessageText(out);
    }
    else out.text = type[0] ? i18n::Text(std::string("[") + type + "]") : "[" + i18n::text("media.message") + "]";
    // Returns the link in the form the UI opens, or "" when it is not an openable link.
    auto normalize_link = [&](const std::string& url) -> std::string {
        std::string normalized = url;
        if (normalized.compare(0, 5, "t.me/") == 0 || normalized.compare(0, 12, "telegram.me/") == 0)
            normalized = "https://" + normalized;
        std::string prefix = normalized.substr(0, 8);
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (prefix.compare(0, 7, "http://") && prefix != "https://" && !is_proxy_link(normalized)) return "";
        if (normalized.find_first_of("\r\n\t ") != std::string::npos) return "";
        return normalized;
    };
    auto add_link = [&](const std::string& url) {
        const std::string normalized = normalize_link(url);
        if (normalized.empty()) return;
        if (std::find(out.links.begin(), out.links.end(), normalized) == out.links.end()) out.links.push_back(normalized);
    };
    if (!std::strcmp(type, "messageRichMessage"))
        rich_message::visit_links(json_object_get(content, "message"), add_link);
    std::string lower = out.text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (size_t pos = 0; pos < lower.size();) {
        size_t next = std::string::npos;
        for (const char* prefix : {"http", "tg:", "t.me/", "telegram.me/", "telegram.dog/proxy?"})
            next = std::min(next, lower.find(prefix, pos));
        if (next == std::string::npos) break;
        pos = next;
        size_t end = out.text.find_first_of(" \t\r\n<>\"", pos);
        if (end == std::string::npos) end = out.text.size();
        std::string url = out.text.substr(pos, end - pos);
        while (!url.empty() && std::string(".,;!").find(url.back()) != std::string::npos) url.pop_back();
        while (!url.empty() && url.back() == ')' && std::count(url.begin(), url.end(), ')') > std::count(url.begin(), url.end(), '(')) url.pop_back();
        add_link(url);
        pos = end;
    }
    if (json_t* forward = json_object_get(msg, "forward_info"); json_is_object(forward)) {
        if (json_t* origin = json_object_get(forward, "origin"); json_is_object(origin)) {
            const char* origin_type = json_str(origin, "@type");
            if (!std::strcmp(origin_type, "messageOriginUser")) { out.forwarded = true; out.forward_origin_id = json_i53(origin, "sender_user_id"); }
            else if (!std::strcmp(origin_type, "messageOriginHiddenUser")) { out.forwarded = true; out.forward_hidden_name = json_str(origin, "sender_name"); }
            else if (!std::strcmp(origin_type, "messageOriginChat")) { out.forwarded = true; out.forward_from_chat = true; out.forward_origin_id = json_i53(origin, "sender_chat_id"); }
            else if (!std::strcmp(origin_type, "messageOriginChannel")) { out.forwarded = true; out.forward_from_chat = true; out.forward_origin_id = json_i53(origin, "chat_id"); }
        }
    }
    // Reply reference: ids now, the preview text is resolved later (history, cache, one lookup).
    if (json_t* reply = json_object_get(msg, "reply_to"); json_is_object(reply) && !std::strcmp(json_str(reply, "@type"), "messageReplyToMessage")) {
        out.reply_to_id = json_i53(reply, "message_id");
        out.reply_chat_id = json_i53(reply, "chat_id");
        if (json_t* quote = json_object_get(reply, "quote"); json_is_object(quote)) out.reply_quote = json_str(json_object_get(quote, "text"), "text");
        if (json_t* origin = json_object_get(reply, "origin"); json_is_object(origin) && !std::strcmp(json_str(origin, "@type"), "messageOriginHiddenUser"))
            out.reply_origin_name = json_str(origin, "sender_name");
        if (json_t* reply_content = json_object_get(reply, "content"); json_is_object(reply_content)) {
            // Present for originals outside this chat/topic: the same parser gives their text or media label.
            json_t* fake = json_pack("{s:O,s:I}", "content", reply_content, "id", (json_int_t)out.reply_to_id);
            if (fake) { out.reply_text = reply_preview_text(parseMessage(fake)); json_decref(fake); }
        }
    }
    json_t* formatted = json_object_get(content, !std::strcmp(type, "messageText") ? "text" : "caption");
    json_t* entities = json_object_get(formatted, "entities");
    if (json_is_array(entities)) {
        // Entity offsets count UTF-16 units; spans are only kept when they index the text shown as-is.
        const bool spans_valid = json_is_object(formatted) && out.text == json_str(formatted, "text", "");
        std::vector<int> unit_to_byte;
        if (spans_valid) {
            for (size_t b = 0; b < out.text.size();) {
                const unsigned char c = (unsigned char)out.text[b];
                const size_t length = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
                unit_to_byte.push_back((int)b);
                if (length == 4) unit_to_byte.push_back((int)b);   // surrogate pair: two units, one character
                b += length;
            }
            unit_to_byte.push_back((int)out.text.size());
        }
        size_t i; json_t* entity;
        json_array_foreach(entities, i, entity) {
            json_t* entity_type = json_object_get(entity, "type");
            const char* kind = json_str(entity_type, "@type");
            const bool text_url = !std::strcmp(kind, "textEntityTypeTextUrl"), plain_url = !std::strcmp(kind, "textEntityTypeUrl");
            if (!text_url && !plain_url) continue;
            int begin = -1, end = -1;
            const int64_t offset = json_i53(entity, "offset"), length = json_i53(entity, "length");
            if (spans_valid && offset >= 0 && length > 0 && (size_t)(offset + length) < unit_to_byte.size()) {
                begin = unit_to_byte[(size_t)offset]; end = unit_to_byte[(size_t)(offset + length)];
            }
            const std::string raw = text_url ? json_str(entity_type, "url") : (begin >= 0 ? out.text.substr(begin, end - begin) : "");
            const std::string normalized = normalize_link(raw);
            if (normalized.empty()) continue;
            add_link(normalized);
            if (begin >= 0 && end > begin) out.link_spans.push_back({begin, end, normalized});
        }
        std::sort(out.link_spans.begin(), out.link_spans.end(), [](const LinkSpan& a, const LinkSpan& b) { return a.begin < b.begin; });
        // Overlapping entities would draw twice.
        for (size_t k = 1; k < out.link_spans.size();)
            if (out.link_spans[k].begin < out.link_spans[k - 1].end) out.link_spans.erase(out.link_spans.begin() + k); else ++k;
    }
    return out;
}

void TelegramClient::requestFile(int32_t file_id, int priority, bool visible_avatar) {
    media_cache_.prioritize(file_id);
    if (!ready_) return;
    if (file_id <= 0) return;
    // The visible row asks again every frame; the check is RAM only.
    if (media_cache_.awaiting(file_id)) return;
    if (visible_avatar) list_avatar_files_.insert(file_id);
    bool fresh = pending_files_.insert(file_id).second;
    if (!fresh && file_priorities_[file_id] >= priority) {
        if (visible_avatar) {
            lock_queue(&queue_mutex_);
            for (auto& queued:outgoing_) if (queued.file_id==file_id) queued.backlog_exempt=true;
            unlock_queue(&queue_mutex_);
        }
        return;
    }
    file_priorities_[file_id] = priority;
    json_t* req = json_pack("{s:s,s:i,s:i,s:I,s:I,s:b}",
        "@type", "downloadFile", "file_id", file_id, "priority", priority,
        "offset", (json_int_t)0, "limit", (json_int_t)0, "synchronous", 0);
    if (visible_avatar) json_object_set_new(req,"@extra",json_pack("{s:s}","kind","visible_avatar"));
    else if (gifsCurrent()) for (size_t i = 0; i < gifs_.items.size(); ++i) {
        if (gifs_.items[i].preview != file_id) continue;
        json_object_set_new(req, "@extra", json_pack("{s:s,s:i,s:i,s:i}", "kind", "preview_download",
            "generation", (int)gifs_.generation, "item", (int)i + 1, "file", file_id));
        break;
    }
    sendJson(req);
}

void TelegramClient::downloadDocument(const MessageItem& message, const std::string& directory) {
    if (!message.document || message.media_file_id <= 0) return;
    std::string dir = normalized_download_dir(directory);
    if (dir.empty()) { document_status_ = i18n::text("doc.invalid_path"); return; }
    if (document_downloads_.count(message.media_file_id)) { document_status_ = i18n::text("doc.already"); return; }
    if (!message.media_path.empty()) {
        std::string result;
        document_status_ = save_downloaded_file(message.media_path, dir, message.document_name,
                                                 message.media_file_id, result) ? i18n::text("doc.saved", {result}) : i18n::text("doc.save_failed");
        return;
    }
    document_downloads_[message.media_file_id] = {dir, message.document_name};
    document_status_ = i18n::text("doc.downloading", {safe_download_name(message.document_name, message.media_file_id)});
    requestFile(message.media_file_id, 32);
}

std::string TelegramClient::uploadStatus() const {
    return uploads_.summary(sceKernelGetSystemTimeWide(), currentChatId());
}
void TelegramClient::logUploads(bool force) {
    uploads_pending_.store(std::any_of(uploads_.items().begin(), uploads_.items().end(),
        [](const MediaUploads::Item& item) { return !item.terminal; }));
    int64_t now = sceKernelGetSystemTimeWide();
    if (!force && now - upload_log_time_ < 2000000) return;
    upload_log_time_ = now;
    std::string snapshot;
    for (const auto& line : uploads_.lines(now, true)) snapshot += line + "\n";
    if (snapshot.empty() || snapshot == upload_log_snapshot_) return;
    upload_log_snapshot_ = snapshot;
    std::string entry = std::to_string(std::time(nullptr)) + " " + snapshot;
    lock_queue(&queue_mutex_);
    // Diagnostic output may be dropped under slow storage; protocol events may not.
    if (upload_logs_.size() >= 64) upload_logs_.pop_front();
    upload_logs_.push_back(std::move(entry));
    unlock_queue(&queue_mutex_);
}
void TelegramClient::flushUploadLogs() {
    std::deque<std::string> lines;
    lock_queue(&queue_mutex_);
    lines.swap(upload_logs_);
    unlock_queue(&queue_mutex_);
    if (lines.empty() || !diag_log::enabled(diag_log::Kind::Upload)) return;
    // Rotate on the cache worker, never on UI/network threads.
    SceIoStat info{};
    const char* path = "ux0:data/vitagram/media-upload.log";
    if (sceIoGetstat(path, &info) >= 0 && info.st_size > 1024 * 1024)
        std::rename(path, "ux0:data/vitagram/media-upload.previous.log");
    if (FILE* log = std::fopen(path, "ab")) {
        for (const auto& line : lines) std::fwrite(line.data(), 1, line.size(), log);
        std::fclose(log);
    }
}

download_stream::Shared TelegramClient::mediaStream(void* opaque, bool live_update) {
    auto* file = static_cast<json_t*>(opaque);
    const int32_t id = (int32_t)json_i53(file, "id");
    auto* local = json_object_get(file, "local");
    if (id <= 0 || !json_is_object(local)) return nullptr;
    const bool complete = json_is_true(json_object_get(local, "is_downloading_completed"));
    auto found = media_streams_.find(id);
    auto stream = found == media_streams_.end() ? nullptr : found->second.lock();
    if (stream && !live_update) return stream; // Reparsed cached messages must not rewind live progress.
    if (!stream && complete) return nullptr; // Completed media keep the existing file/decryption path.
    if (!stream) {
        if (media_streams_.size() >= 128) {
            for (auto it = media_streams_.begin(); it != media_streams_.end();)
                if (it->second.expired()) it = media_streams_.erase(it); else ++it;
        }
        stream = std::make_shared<download_stream::State>(id);
        media_streams_[id] = stream;
    }
    // expected_size and downloaded_size are estimates/progress, not readable extents.
    stream->update(json_str(local, "path"), json_i53(file, "size"), json_i53(local, "download_offset"),
                   json_i53(local, "downloaded_prefix_size"), complete);
    return stream;
}

void TelegramClient::updateFile(void* opaque, bool stream_update) {
    json_t* file = (json_t*)opaque;
    int32_t id = (int32_t)json_i53(file, "id");
    json_t* local = json_object_get(file, "local");
    const char* path = json_is_object(local) ? json_str(local, "path") : "";
    if (id <= 0) return;
    if (stream_update && media_streams_.count(id)) mediaStream(file, true);
    if (path[0] && json_is_true(json_object_get(local, "is_downloading_completed"))) sealer_.enqueue(path);
    auto document_job = document_downloads_.find(id);
    if (document_job != document_downloads_.end() && json_is_true(json_object_get(local, "is_downloading_completed")) && path[0]) {
        std::string result;
        document_status_ = save_downloaded_file(path, document_job->second.directory, document_job->second.name,
                                                 id, result) ? i18n::text("doc.saved", {result}) : i18n::text("doc.save_failed");
        document_downloads_.erase(document_job);
    }
    auto update_document = [&](MessageItem& message) {
        if (message.document && message.media_file_id == id) {
            message.document_downloaded = json_i53(local, "downloaded_size");
            if (document_downloads_.count(id)) {
                if (message.document_size > 0)
                    document_status_ = i18n::text("doc.downloading", {std::to_string(std::min<int64_t>(99,
                        message.document_downloaded * 100 / message.document_size)) + "%"});
                else document_status_ = i18n::text("doc.downloading", {i18n::text("unit.kib", {std::to_string(message.document_downloaded / 1024)})});
            }
        }
    };
    for (auto& message : messages_) update_document(message);
    for (auto& message : history_items_) update_document(message);
    for (auto& photo : user_photos_) if (photo.second.file == id)
        photo.second.path = json_is_true(json_object_get(local, "is_downloading_completed")) ? path : "";

    for (auto& sticker : stickers_) if (sticker.preview == id) sticker.path = path;
    for (size_t i = 0; i < gifs_.items.size(); ++i) {
        auto& gif = gifs_.items[i];
        if (gif.preview != id) continue;
        auto next = json_is_true(json_object_get(local, "is_downloading_completed")) ? GifCatalog::previewPath(gif, path) : "";
        if (!next.empty() && gif.path != next)
            gif_diagnostics::preview("preview_downloaded", gif.preview_format.c_str(), gifs_.generation, i + 1, id,
                gif.preview_width, gif.preview_height, std::max(json_i53(file, "size"), json_i53(local, "downloaded_size")), "ok", performance::now());
        gif.path = std::move(next);
    }
    for (auto& pack : sticker_packs_) if (pack.icon_file_id == id) pack.icon_path = path;
    auto update_voice = [&](MessageItem& message) {
        if (message.voice_file_id != id) return;
        if (!message.voice_stream) message.voice_stream = mediaStream(file);
        message.voice_size = std::max(json_i53(file, "size"), json_i53(file, "expected_size"));
        message.voice_downloaded = json_i53(local, "downloaded_size");
        if (std::max(message.voice_size, message.voice_downloaded) > (message.audio ? 128 : 20) * 1024 * 1024)
            message.voice_error = message.audio ? i18n::text("voice.limit_audio") : i18n::text("voice.limit_voice");
        if (json_is_true(json_object_get(local, "is_downloading_completed"))) message.voice_path = path;
    };
    for (auto& message : messages_) update_voice(message);
    for (auto& message : history_items_) update_voice(message);
    auto update_animation = [&](MessageItem& message) {
        if (message.animation_file_id != id) return;
        if (message.video && !message.animation_stream) message.animation_stream = mediaStream(file);
        message.animation_size = std::max(json_i53(file, "size"), json_i53(file, "expected_size"));
        message.animation_downloaded = json_i53(local, "downloaded_size");
        if (std::max(message.animation_size, message.animation_downloaded) > (message.video ? 128 : 20) * 1024 * 1024)
            message.animation_error = message.video ? i18n::text("anim.limit_video") : i18n::text("anim.limit_gif");
        if (json_is_true(json_object_get(local, "is_downloading_completed"))) {
            message.animation_path = path;
            if (message.video && id == active_animation_file_ && animation_started_at_) {
                performance::observe(performance::Metric::VideoReady, performance::now()-animation_started_at_);
                animation_started_at_ = 0;
            }
        }
    };
    for (auto& message : messages_) update_animation(message);
    for (auto& message : history_items_) update_animation(message);
    for (auto& entry:reaction_previews_) if (entry.second.file==id)
        entry.second.path=json_is_true(json_object_get(local,"is_downloading_completed")) ? path : "";
    if (!json_is_true(json_object_get(local, "is_downloading_completed"))) {
        // Chat avatars keep their last picture while TDLib (re)downloads the file.
        auto clear_path = [&](MessageItem& message) {
            if (message.media_file_id == id) message.media_path.clear();
            if (message.voice_file_id == id) message.voice_path.clear();
            if (message.animation_file_id == id) message.animation_path.clear();
            for (auto& photo : message.photos) {
                if (photo.thumbnail_file_id == id) photo.thumbnail_path.clear();
                if (photo.full_file_id == id) photo.full_path.clear();
            }
        };
        for (auto& message : messages_) clear_path(message);
        for (auto& message : history_items_) clear_path(message);
        return;
    }
    if (!path[0]) return;
    pending_files_.erase(id);
    for (auto& chat : chats_) if (chat.avatar_file_id == id) { chat.avatar_path = path; chat.avatar_stale = false; }
    for (auto& chat : known_chats_) if (chat.avatar_file_id == id) {
        // A new picture changes the startup index even when the list itself does not.
        if (chat.avatar_path != path) startup_list_dirty_ = true;
        chat.avatar_path = path; chat.avatar_stale = false;
    }
    for (auto& message : messages_) {
        if (message.media_file_id == id) message.media_path = GifCatalog::previewPath(message.media_video_preview, path);
        for (auto& photo : message.photos) {
            if (photo.thumbnail_file_id == id) photo.thumbnail_path = path;
            if (photo.full_file_id == id) photo.full_path = path;
        }
    }
    for (auto& message : history_items_) {
        if (message.media_file_id == id) message.media_path = GifCatalog::previewPath(message.media_video_preview, path);
        for (auto& photo : message.photos) {
            if (photo.thumbnail_file_id == id) photo.thumbnail_path = path;
            if (photo.full_file_id == id) photo.full_path = path;
        }
    }
}

void TelegramClient::ensureChatMedia(size_t index) {
    performance::SlowScope slow(performance::SlowPhase::Ui,"ensure_chat_media");
    if (index >= chats_.size()) return;
    const auto& chat = chats_[index];
    if (chat.cached_metadata && chat.avatar_path.empty() && cache_restore_allowed_) {
        lock_queue(&queue_mutex_);
        if (cached_avatar_requests_.size() + cached_avatar_results_.size() < 32 &&
            !cached_avatar_seen_.count(chat.id)) {
            if (cached_avatar_seen_.size() >= 2048) cached_avatar_seen_.clear();
            cached_avatar_seen_.insert(chat.id); cached_avatar_requests_.push_back(chat.id);
        }
        unlock_queue(&queue_mutex_);
    }
    // Restored file IDs are intentionally zero. loadChats may return only list
    // positions, so explicitly refresh metadata for visible cached rows.
    if (ready_ && chat.cached_metadata) {
        int64_t now = sceKernelGetSystemTimeWide();
        if (now >= avatar_metadata_retry_[chat.id]) {
            avatar_metadata_retry_[chat.id] = now + 10000000;
            json_t* request = json_pack("{s:s,s:I,s:s}", "@type", "getChat", "chat_id", (json_int_t)chat.id,
                "@extra", "avatar_metadata");
            sendJson(request);
        }
    }
    if (chat.avatar_path.empty() || chat.avatar_stale) requestFile(chat.avatar_file_id, 8,true);
}

void TelegramClient::ensureMessageMedia(size_t index) {
    performance::SlowScope slow(performance::SlowPhase::Ui,"ensure_message_media");
    if (index >= messages_.size()) return;
    const auto& message = messages_[index];
    if (ready_ && message.member_removed) {
        for (int64_t user : {message.sender_chat ? int64_t(0) : message.sender_id, message.removed_member_id}) {
            if (!user || user_names_.count(user)) continue;
            std::string key = "u" + std::to_string(user);
            int64_t now = sceKernelGetSystemTimeWide();
            if (now < sender_requests_[key]) continue;
            sender_requests_[key] = now + 10000000;
            sendJson(json_pack("{s:s,s:I,s:s}", "@type", "getUser", "user_id", (json_int_t)user, "@extra", "sender_lookup"));
        }
    }
    ensureSenderMedia(message);
    if (!message.photos.empty()) {
        ensurePhotoThumbnail(index, 0);
    } else if (!message.document && message.media_path.empty()) requestFile(message.media_file_id, 16);
}

void TelegramClient::setAnimationTarget(int index, bool spoiler_revealed) {
    const MessageItem* message = index >= 0 && index < (int)messages_.size() ? &messages_[index] : nullptr;
    int32_t wanted = message && (!message->spoiler || spoiler_revealed) && message->animation && message->animation_error.empty() ? message->animation_file_id : 0;
    if (wanted != active_animation_file_) {
        if (active_animation_file_ && pending_files_.erase(active_animation_file_))
            sendJson(json_pack("{s:s,s:i,s:b}", "@type", "cancelDownloadFile", "file_id", active_animation_file_, "only_if_pending", 0));
        active_animation_file_ = wanted;
        animation_started_at_ = 0; animation_progress_ = 0; animation_progress_at_ = 0;
    }
    if (wanted && message->animation_path.empty()) {
        const int64_t now = sceKernelGetSystemTimeWide();
        // The open video is what the user waits for: keep background downloads
        // in the app queue so the single TDLib thread and the link serve it.
        foreground_until_.store(now + 2000000);
        if (message->animation_downloaded != animation_progress_ || !animation_progress_at_) {
            animation_progress_ = message->animation_downloaded; animation_progress_at_ = now;
        } else if (now - animation_progress_at_ > 20000000 && pending_files_.erase(wanted)) {
            // No progress for a long time: TDLib may have dropped the job. A repeated downloadFile is harmless.
            animation_progress_at_ = now;
        }
    } else { animation_progress_ = 0; animation_progress_at_ = 0; }
    if (wanted && message->animation_path.empty() && pending_files_.insert(wanted).second) {
        ++animation_download_generation_;
        if (!animation_started_at_) animation_started_at_ = performance::now();
        json_t* request = json_pack("{s:s,s:i,s:i,s:I,s:I,s:b}", "@type", "downloadFile", "file_id", wanted,
            "priority", 32, "offset", (json_int_t)0, "limit", (json_int_t)0, "synchronous", 0);
        json_object_set_new(request, "@extra", json_pack("{s:s,s:i,s:I}", "kind", "animation_download", "file", wanted,
            "generation", (json_int_t)animation_download_generation_));
        sendJson(request);
    }
}

void TelegramClient::setVoiceTarget(int index) {
    const MessageItem* message = index >= 0 && index < (int)messages_.size() ? &messages_[index] : nullptr;
    int32_t wanted = message && message->voice && message->voice_error.empty() ? message->voice_file_id : 0;
    if (wanted != active_voice_file_) {
        if (active_voice_file_ && pending_files_.erase(active_voice_file_))
            sendJson(json_pack("{s:s,s:i,s:b}", "@type", "cancelDownloadFile", "file_id", active_voice_file_, "only_if_pending", 0));
        active_voice_file_ = wanted;
    }
    if (wanted && message->voice_path.empty() && pending_files_.insert(wanted).second) {
        ++voice_download_generation_;
        json_t* request = json_pack("{s:s,s:i,s:i,s:I,s:I,s:b}", "@type", "downloadFile", "file_id", wanted,
            "priority", 32, "offset", (json_int_t)0, "limit", (json_int_t)0, "synchronous", 0);
        json_object_set_new(request, "@extra", json_pack("{s:s,s:i,s:I}", "kind", "voice_download", "file", wanted,
            "generation", (json_int_t)voice_download_generation_));
        sendJson(request);
    }
}

void TelegramClient::ensurePhotoThumbnail(size_t message_index, size_t photo_index) {
    if (message_index >= messages_.size() || photo_index >= messages_[message_index].photos.size()) return;
    const auto& photo = messages_[message_index].photos[photo_index];
    if (photo.thumbnail_path.empty()) requestFile(photo.thumbnail_file_id, 16);
}

void TelegramClient::ensureFullPhoto(size_t message_index, size_t photo_index) {
    if (message_index >= messages_.size() || photo_index >= messages_[message_index].photos.size()) return;
    const auto& photo = messages_[message_index].photos[photo_index];
    if (photo.full_path.empty()) requestFile(photo.full_file_id, 32);
}

void TelegramClient::process(void* opaque) {
    performance::Scope measurement(performance::Metric::Process);
    json_t* root = static_cast<json_t*>(opaque);
    if (!root) return;
    const uint64_t wire_bytes=td_event_wire_bytes(root);
    performance::SlowScope slow_process(performance::SlowPhase::Process,json_str(root,"@type"),wire_bytes);
    // Ignore late account responses once logout starts; they must not repopulate caches.
    if (logging_out_) {
        if (!std::strcmp(json_str(root, "@type"), "updateAuthorizationState")) processAuth(root);
        retire_td_json(root,wire_bytes); return;
    }
    json_t* list_extra = json_object_get(root, "@extra");
    const char* request_kind = json_str(list_extra, "kind");
    const bool refresh_reply = !std::strcmp(request_kind, "refresh_main_ids") || !std::strcmp(request_kind, "refresh_main_chat");
    if ((refresh_reply || !std::strcmp(request_kind, "load_main_chats")) &&
        (json_i53(list_extra, "list_generation") != list_generation_ ||
         (refresh_reply && json_object_get(list_extra, "refresh_generation") &&
          json_i53(list_extra, "refresh_generation") != refresh_generation_))) {
        // Old replies must not overwrite newer metadata or complete a retry.
        retire_td_json(root,wire_bytes); return;
    }
    const int64_t received_at = json_i53(root, "@received_at");
    const int64_t event_time = received_at > 0 ? received_at : sceKernelGetSystemTimeWide();
    const char* event_type = json_str(root, "@type");
    json_t* event_file = !std::strcmp(event_type, "updateFile") ? json_object_get(root, "file") :
        !std::strcmp(event_type, "file") ? root : nullptr;
    if (event_file) {
        const int32_t id = (int32_t)json_i53(event_file, "id");
        if (id && file_update_times_[id] > event_time) { retire_td_json(root,wire_bytes); return; }
        if (id) file_update_times_[id] = event_time;
        // Publish original TDLib extents before the asynchronous cache validator
        // temporarily hides an unverified completed path from the UI.
        if (media_streams_.count(id)) mediaStream(event_file, true);
    } else {
        json_t* message = !std::strcmp(event_type, "message") ? root : json_object_get(root, "message");
        const int32_t id = (int32_t)json_i53(MediaUploads::messageFile(message), "id");
        if (id) file_update_times_[id] = std::max(file_update_times_[id], event_time);
    }
    uploads_.process(root, event_time);
    json_object_del(root, "@received_at");
    json_object_del(root,"@parsed_at");
    json_object_del(root, "@wire_bytes");
    logUploads();
    const bool sticker_payload=!std::strcmp(event_type,"stickerSet") || !std::strcmp(event_type,"updateStickerSet") || !std::strcmp(event_type,"stickers") ||
        !std::strncmp(request_kind, "gif_", 4);
    if (!sticker_payload) for (auto id : media_cache_.enqueue(root)) pending_files_.erase(id);
    // Retain TD's original file metadata for future background/cache resolves.
    // A temporarily unverified UI path must not erase it from persisted history.
    ui_cache_.remember(root,wire_bytes);
    if (!sticker_payload) media_cache_.resolve(root, false);

    const char* type = json_str(root, "@type");
    if (!std::strncmp(request_kind, "gif_", 4)) {
        if (!gifs_.matches(list_extra, current_chat_id_, current_topic_, current_thread_) || !ready_) {
            gif_diagnostics::record("ignored", request_kind, json_i53(list_extra, "generation"), performance::now());
            retire_td_json(root, wire_bytes); return;
        }
        gif_diagnostics::record("accepted", request_kind, gifs_.generation, performance::now());
        if (!std::strcmp(type, "error")) {
            gifs_.finish();
            gif_diagnostics::record("error", request_kind, gifs_.generation, performance::now(), json_i53(root, "code"));
            gif_status_ = i18n::text("gif.error", {json_str(root, "message")});
        } else if (!std::strcmp(request_kind, "gif_option") && !std::strcmp(type, "optionValueString") && *json_str(root, "value")) {
            auto* request = json_pack("{s:s,s:s}", "@type", "searchPublicChat", "username", json_str(root, "value"));
            json_object_set_new(request, "@extra", gifs_.extra("gif_bot")); sendJson(request);
        } else if (!std::strcmp(request_kind, "gif_bot") && !std::strcmp(type, "chat")) {
            gif_bot_ = json_i53(json_object_get(root, "type"), "user_id");
            if (gif_bot_) requestGifResults("");
            else { gifs_.finish(); gif_status_ = i18n::text("gif.unavailable"); }
        } else if ((!std::strcmp(request_kind, "gif_saved") && !std::strcmp(type, "animations")) ||
                   (!std::strcmp(request_kind, "gif_results") && !std::strcmp(type, "inlineQueryResults"))) {
            const bool saved = !std::strcmp(request_kind, "gif_saved");
            json_t* array = json_object_get(root, saved ? "animations" : "results");
            size_t index; json_t* result;
            json_array_foreach(array, index, result) {
                json_t* animation = saved ? result : json_object_get(result, "animation");
                json_t* preview = GifCatalog::previewFile(animation);
                for (auto id : media_cache_.enqueue(preview)) pending_files_.erase(id);
                media_cache_.resolve(preview, false);
            }
            gifs_.append(root, saved, json_str(list_extra, "offset"));
            gif_diagnostics::record("completed", request_kind, gifs_.generation, performance::now(), gifs_.items.size());
            gif_status_ = gifs_.items.empty() ? i18n::text(saved ? "gif.no_saved" : "gif.no_results") : i18n::text("gif.choose");
        } else { gifs_.finish(); gif_status_ = i18n::text("gif.unavailable"); }
        retire_td_json(root, wire_bytes); return;
    }
    if (!std::strcmp(type, "updateConnectionState")) {
        const bool connected = !std::strcmp(json_str(json_object_get(root, "state"), "@type"), "connectionStateReady");
        if (connected && !connection_ready_) resetListRequests(sceKernelGetSystemTimeWide());
        connection_ready_ = connected;
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(type, "updateUserStatus")) {
        parseUserStatus(json_i53(root, "user_id"), json_object_get(root, "status"));
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(root, "@extra"), "cache_cleanup")) {
        cache_maintenance_ = false;
        if (!std::strcmp(type, "error")) error_ = std::string("Cache: ") + json_str(root, "message");
        else media_cache_.invalidate();
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(type, "user") || !std::strcmp(type, "updateUser")) {
        if (ready_ && !std::strcmp(json_str(root, "@extra"), "account_identity")) {
            account_user_id_ = json_i53(root, "id");
            delete_permissions_.clear();
        }
        parseUser(!std::strcmp(type, "user") ? root : json_object_get(root, "user"));
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(type, "error") && !std::strcmp(json_str(root, "@extra"), "sender_lookup")) {
        retire_td_json(root,wire_bytes); return;
    }
    json_t* extra = json_object_get(root, "@extra");
    if (!std::strcmp(json_str(extra, "kind"), "delete_permission")) {
        if (json_i53(extra, "chat") == current_chat_id_ && json_i53(extra, "generation") == history_generation_) {
            auto found = delete_permissions_.find(json_i53(extra, "message"));
            if (found != delete_permissions_.end()) {
                const bool saved = account_user_id_ > 0 && current_chat_id_ == account_user_id_ && currentChatIsPrivate();
                const bool properties = !std::strcmp(type, "messageProperties");
                const auto permission = message_delete::permission(saved,
                    properties && json_is_true(json_object_get(root, "can_be_deleted_for_all_users")),
                    properties && json_is_true(json_object_get(root, "can_be_deleted_only_for_self")));
                found->second.pending = false;
                found->second.allowed = permission.allowed;
                found->second.revoke = permission.revoke;
            }
        }
        retire_td_json(root, wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "delete_message")) {
        if (json_i53(extra, "chat") == current_chat_id_) {
            message_delete_pending_ = false; message_delete_deadline_ = 0;
            if (!std::strcmp(type, "error")) {
                const std::string error = json_str(root, "message");
                i18n::Text reason = i18n::text("message.delete_other");
                if (error.find("FORBIDDEN") != std::string::npos || error.find("ADMIN_REQUIRED") != std::string::npos)
                    reason = i18n::text("message.delete_rights");
                else if (error.find("NOT_FOUND") != std::string::npos || error.find("MESSAGE_ID_INVALID") != std::string::npos)
                    reason = i18n::text("message.delete_missing");
                else if (json_i53(root, "code") == 429) reason = i18n::text("message.delete_retry");
                notice_ = i18n::text("message.delete_failed", {reason + " (" + error + ")"});
                delete_permissions_.clear();
            } else {
                auto* update = json_pack("{s:s,s:I,s:O,s:b,s:b}", "@type", "updateDeleteMessages",
                    "chat_id", (json_int_t)current_chat_id_, "message_ids", json_object_get(extra, "ids"),
                    "is_permanent", 1, "from_cache", 0);
                ui_cache_.remember(update);
                size_t index; json_t* id;
                json_array_foreach(json_object_get(extra, "ids"), index, id) {
                    const int64_t deleted = json_integer_value(id);
                    history_items_.erase(std::remove_if(history_items_.begin(), history_items_.end(),
                        [&](const MessageItem& item) { return item.id == deleted; }), history_items_.end());
                    delete_permissions_.erase(deleted);
                    if (reply_target_ == deleted) reply_target_ = 0;
                    if (forward_chat_ == current_chat_id_ && forward_message_ == deleted) cancelForward();
                }
                json_decref(update);
                rebuildHistory();
                notice_ = i18n::text("message.deleted");
            }
            notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
        }
        retire_td_json(root, wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "preview_download") && !std::strcmp(type, "error")) {
        gif_diagnostics::preview("preview_download_error", "download", json_i53(extra, "generation"),
            (int)json_i53(extra, "item"), (int)json_i53(extra, "file"), 0, 0, 0, "td_error", performance::now(), json_i53(root, "code"));
        retire_td_json(root, wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "channel_full")) {
        // Linked discussion now known to TDLib: ask again for the posts that came without reply_info.
        if (!std::strcmp(type, "supergroupFullInfo") && json_i53(root, "linked_chat_id") && json_i53(extra, "chat") == current_chat_id_) {
            comment_probed_.clear();
            probeComments();
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "comments_probe")) {
        // The server has been asked about the posts; read them back with whatever reply_info TDLib now holds.
        if (json_i53(extra, "chat") == current_chat_id_ && json_i53(extra, "generation") == (json_int_t)history_generation_ && !current_thread_) {
            auto* request = json_pack("{s:s,s:I,s:O}", "@type", "getMessages", "chat_id", (json_int_t)current_chat_id_, "message_ids", json_object_get(extra, "ids"));
            json_object_set_new(request, "@extra", json_pack("{s:s,s:I,s:I}", "kind", "comments_refresh", "chat", (json_int_t)current_chat_id_, "generation", (json_int_t)history_generation_));
            sendJson(request);
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "comments_refresh")) {
        json_t* list = json_object_get(root, "messages");
        if (json_i53(extra, "chat") == current_chat_id_ && json_i53(extra, "generation") == (json_int_t)history_generation_ && !current_thread_ && json_is_array(list)) {
            size_t i; json_t* m;
            json_array_foreach(list, i, m) if (json_is_object(m) && json_is_object(json_object_get(m, "interaction_info")) && json_i53(m, "chat_id") == current_chat_id_) {
                const int64_t id = json_i53(m, "id");
                for (auto& item : history_items_) if (item.id == id) parse_comments(item, json_object_get(m, "interaction_info"));
            }
            rebuildHistory();
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "comments_open")) {
        if (json_i53(extra, "chat") == current_chat_id_ && json_i53(extra, "generation") == history_generation_ && !current_thread_) {
            const int64_t discussion = json_i53(root, "chat_id"), thread = json_i53(root, "message_thread_id");
            if (!std::strcmp(type, "messageThreadInfo") && discussion && thread) {
                pending_return_ = {current_chat_id_, current_chat_title_, json_i53(extra, "message"), json_is_true(json_object_get(extra, "post"))};
                pending_thread_ = thread;
                link_chat_opened_ = true;   // the UI drops the expanded post / selection of the channel
                openChat(discussion, current_chat_title_ + " / " + i18n::text("comments.title"));
            } else {
                const bool known = !std::strcmp(type, "error");
                notice_ = i18n::text("comments.unavailable", {known && *json_str(root, "message") ? i18n::Text(json_str(root, "message")) : i18n::text("comments.restricted")});
                notice_until_ = sceKernelGetSystemTimeWide() + 8000000;
            }
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "pin_target")) {
        if (json_i53(extra, "chat") == current_chat_id_ && json_i53(extra, "generation") == history_generation_) {
            if (!std::strcmp(type, "message") && json_i53(root, "chat_id") == current_chat_id_ && matchesTopic(root)) {
                storeMessage(root);
                openPinnedMessage(json_i53(root, "id"));
            } else {
                notice_ = i18n::text("link.open_failed", {*json_str(root, "message") ? i18n::Text(json_str(root, "message")) : i18n::text("link.chat_unavailable")});
                notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
            }
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "reply_preview")) {
        const std::pair<int64_t, int64_t> key{json_i53(extra, "chat"), json_i53(extra, "message")};
        ReplyInfo info;
        if (!std::strcmp(type, "message")) {
            const MessageItem item = parseMessage(root);
            info.sender_id = item.sender_id; info.sender_chat = item.sender_chat;
            info.text = reply_preview_text(item);
            if (info.text.empty()) info.text = i18n::text("media.message");
        } else info.missing = true;   // deleted, or not accessible
        if (reply_cache_.size() > 256) reply_cache_.clear();
        reply_cache_[key] = info;
        reply_requested_.erase(key);
        if (key.first == current_chat_id_) rebuildHistory();
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "speech_request")) {
        if (!std::strcmp(type, "error") && json_i53(extra, "chat") == current_chat_id_) {
            for (auto& item : history_items_) if (item.id == json_i53(extra, "message")) item.transcription_pending = false;
            rebuildHistory();
            notice_ = i18n::text("post.transcription_failed", {json_str(root, "message")});
            notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
            setTranscriptionStatus(notice_, 12);
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "group_status")) {
        if (!std::strcmp(type, "supergroup")) {
            supergroup_member_[json_i53(root, "id")] = membershipOf(json_object_get(root, "status"));
            supergroup_rights_[json_i53(root, "id")] = postRightsOf(json_object_get(root, "status"));
        } else if (!std::strcmp(type, "basicGroup")) {
            basic_group_member_[json_i53(root, "id")] = membershipOf(json_object_get(root, "status"));
            basic_group_rights_[json_i53(root, "id")] = postRightsOf(json_object_get(root, "status"));
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "group_action")) {
        const bool joining = !std::strcmp(json_str(extra, "action"), "join");
        const int64_t chat_id = json_i53(extra, "chat");
        membership_pending_ = false;
        if (!std::strcmp(type, "error")) {
            notice_ = i18n::text(joining ? "notice.join_failed" : "notice.leave_failed", {json_str(root, "message")});
        } else {
            for (const auto& chat : known_chats_) if (chat.id == chat_id) {
                const Membership state = joining ? Membership::Member : Membership::NotMember;
                if (chat.supergroup_id) supergroup_member_[chat.supergroup_id] = state;
                else if (chat.basic_group_id) basic_group_member_[chat.basic_group_id] = state;
                break;
            }
            notice_ = i18n::text(joining ? "notice.joined" : "notice.left");
            if (!joining && chat_id == current_chat_id_) leaveChat();
        }
        notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "history_action")) {
        const bool removed = !std::strcmp(json_str(extra, "action"), "delete");
        membership_pending_ = false;
        if (!std::strcmp(type, "error")) notice_ = i18n::text("notice.action_failed", {json_str(root, "message")});
        else {
            notice_ = i18n::text(removed ? "notice.deleted" : "notice.cleared");
            if (json_i53(extra, "chat") == current_chat_id_) {
                if (removed) leaveChat();
                else { history_items_.clear(); rebuildHistory(); refreshHistory(); }
            }
        }
        notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "search_chats")) {
        if ((unsigned)json_i53(extra, "generation") == search_generation_) {
            if (search_pending_ > 0) --search_pending_;
            json_t* ids = json_object_get(root, "chat_ids");
            const int source = std::max(0, std::min(2, (int)json_i53(extra, "source")));
            if (!std::strcmp(type, "chats") && json_is_array(ids)) {
                size_t i; json_t* v;
                json_array_foreach(ids, i, v) {
                    const int64_t id = json_is_integer(v) ? json_integer_value(v) : 0;
                    if (!id) continue;
                    search_ids_[source].push_back(id);
                    if (std::none_of(known_chats_.begin(), known_chats_.end(), [&](const ChatItem& c) { return c.id == id; }))
                        sendJson(json_pack("{s:s,s:I}", "@type", "getChat", "chat_id", (json_int_t)id));
                }
            }
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "public_link")) {
        if (json_i53(extra, "generation") == link_generation_) {
            if (!std::strcmp(type, "chat")) {
                parseChat(root);
                openChat(json_i53(root, "id"), json_str(root, "title"));
                link_chat_opened_ = true;
            } else {
                notice_ = i18n::text("link.open_failed", {*json_str(root, "message") ? i18n::Text(json_str(root, "message")) : i18n::text("link.chat_unavailable")});
                notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
            }
        }
        retire_td_json(root,wire_bytes); return;
    }
    const std::string poll_kind = json_str(extra, "kind");
    if (poll_kind == "poll_vote" || poll_kind == "poll_refresh") {
        const auto key = std::make_pair(json_i53(extra, "chat"), json_i53(extra, "message"));
        auto& request = poll_requests_[key];
        request.pending = false;
        if (!std::strcmp(type, "error")) request.status = i18n::text("poll.error", {json_str(root, "message")});
        else if (poll_kind == "poll_vote") {
            request.pending = true; request.status = i18n::text("poll.vote_accepted");
            json_t* refresh = json_pack("{s:s,s:I,s:I}", "@type", "getMessage", "chat_id", (json_int_t)key.first, "message_id", (json_int_t)key.second);
            json_object_set_new(refresh, "@extra", json_pack("{s:s,s:I,s:I}", "kind", "poll_refresh", "chat", (json_int_t)key.first, "message", (json_int_t)key.second));
            sendJson(refresh);
        } else request.status = i18n::text("poll.updated");
        if (std::strcmp(type, "message")) { retire_td_json(root,wire_bytes); return; }
    }
    if (!std::strcmp(type, "updatePoll")) {
        PollData poll = PollData::parse(json_object_get(root, "poll"));
        for (auto* list : {&history_items_, &messages_}) for (auto& m : *list) if (poll.id && m.poll.id == poll.id) {
            m.poll = poll; m.media_label = poll.quiz ? i18n::text("media.quiz") : i18n::text("media.poll"); m.text = poll.question;
        }
        retire_td_json(root,wire_bytes); return;
    }

    if (!std::strcmp(json_str(extra, "kind"), "proxy_check")) {
        if (proxy_check_deadline_ && json_i53(extra, "generation") == proxy_check_generation_) {
            proxy_check_deadline_ = 0;
            if (!std::strcmp(type, "seconds")) {
                char result[128];
                std::snprintf(result, sizeof(result), "%.0f", json_number_value(json_object_get(root, "seconds")) * 1000);
                proxy_check_status_ = i18n::text("proxy.check.ok", {result});
            } else proxy_check_status_ = i18n::text("proxy.check.fail");
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "db_key")) {
        database_key_migrating_ = false;
        if (!std::strcmp(type, "ok")) {
            const int marker = (int)json_i53(extra, "marker");
            // Write the new marker before dropping the old one: a crash in between must never
            // leave the database looking unencrypted.
            const char* paths[] = {nullptr, kMarkerConsole, kMarkerVault};
            if (paths[marker]) if (FILE* f = std::fopen(paths[marker], "wb")) { std::fputs("1", f); std::fclose(f); }
            for (int i = 1; i <= 2; ++i) if (i != marker) std::remove(paths[i]);
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "auth_input")) {
        // A rejected number/code/password must not leave the sign-in screen without a prompt.
        if (!std::strcmp(type, "error")) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s (%lld)", json_str(root, "message"), (long long)json_i53(root, "code"));
            const auto kind = static_cast<InputKind>(json_i53(extra, "input"));
            setInput(kind, buf);
            error_.clear();
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "tdlib_params")) {
        if (!std::strcmp(type, "error") && databaseMarker()) {
            status_ = i18n::text("tg.db_key_error"); error_ = status_;
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "proxy_config")) {
        if (json_i53(extra, "generation") == proxy_generation_)
            proxy_status_ = !std::strcmp(type, "error") ? i18n::text("proxy.status.error") :
                (proxy_enabled_ ? i18n::text("proxy.status.enabled") : i18n::text("proxy.status.direct"));
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "forum_check") || !std::strcmp(json_str(extra, "kind"), "forum_topics")) {
        if (json_i53(extra, "chat") == current_chat_id_ && json_i53(extra, "generation") == topics_generation_) {
            if (!std::strcmp(json_str(extra,"kind"),"forum_check") && json_i53(extra,"requested_at")>0)
                performance::observe(performance::Metric::ForumReady,performance::now()-json_i53(extra,"requested_at"));
            topics_pending_ = false;
            if (!std::strcmp(type, "error")) topics_status_ = json_str(root, "message");
            else if (!std::strcmp(json_str(extra, "kind"), "forum_check")) {
                const bool forum=json_is_true(json_object_get(root, "is_forum"));
                if (json_i53(root,"id")) supergroup_forum_[json_i53(root,"id")]=forum;
                // The first history page was requested in parallel. A forum
                // invalidates it by generation; an ordinary group keeps it.
                if (forum) { showTopicsList(); loadTopics(true); }
                else if (topics_open_) { topics_open_ = false; refreshHistory(); }
            } else {
                json_t* arr = json_object_get(root, "topics"); size_t i; json_t* entry;
                json_array_foreach(arr, i, entry) {
                    json_t* info = json_object_get(entry, "info");
                    ForumTopicItem item;
                    item.id = (int32_t)json_i53(info, "forum_topic_id"); item.title = json_str(info, "name");
                    item.unread = (int)json_i53(entry, "unread_count"); item.closed = json_is_true(json_object_get(info, "is_closed"));
                    if (!item.id || json_is_true(json_object_get(info, "is_hidden"))) continue;
                    auto found = std::find_if(topics_.begin(), topics_.end(), [&](const ForumTopicItem& t) { return t.id == item.id; });
                    if (found == topics_.end()) topics_.push_back(item); else *found = item;
                }
                int32_t date = (int32_t)json_i53(root, "next_offset_date"), offset = (int32_t)json_i53(root, "next_offset_forum_topic_id");
                int64_t message = json_i53(root, "next_offset_message_id");
                topics_end_ = !json_array_size(arr) || (!date && !message && !offset) ||
                    (date == topics_date_ && message == topics_message_ && offset == topics_offset_);
                topics_date_ = date; topics_message_ = message; topics_offset_ = offset;
                topics_status_ = topics_.empty() ? i18n::text("topics.none") : topics_end_ ? i18n::text("topics.all_loaded") : i18n::Text();
            }
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "sticker_pack_icon")) {
        if (json_i53(extra, "session") == sticker_session_ && !std::strcmp(type, "stickerSet") &&
            json_i53(root, "id") == json_i53(extra, "set")) {
            auto it = std::find(sticker_set_ids_.begin()+std::min<size_t>(2, sticker_set_ids_.size()),
                                sticker_set_ids_.end(), json_i53(extra, "set"));
            if (it != sticker_set_ids_.end()) {
                size_t index = it - sticker_set_ids_.begin();
                auto* preview=sticker_pack_preview(root);
                for (auto id:media_cache_.enqueue(preview)) pending_files_.erase(id);
                media_cache_.resolve(preview,false);
                applyStickerPackIcon(index,root);
            }
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "sticker_packs") ||
        !std::strcmp(json_str(extra, "kind"), "sticker_items")) {
        bool packs = !std::strcmp(json_str(extra, "kind"), "sticker_packs");
        if (json_i53(extra, "chat") == current_chat_id_ &&
            json_i53(extra, "session") == sticker_session_ &&
            (packs || json_i53(extra, "generation") == sticker_generation_)) {
            if (!packs) { sticker_items_pending_set_=0;sticker_selection_pending_=false;sticker_selection_loaded_=std::strcmp(type,"error")!=0; }
            if (!std::strcmp(type, "error")) {
                sticker_status_ = json_str(root, "message");
                if (packs) sticker_packs_fresh_ = false;
            } else if (packs) {
                sticker_packs_fresh_ = true;
                if (!json_object_get(root,"@ui_saved")) ui_cache_.saveStickerPacks(root);
                sticker_packs_ = {{0, i18n::text("sticker.favorites")}, {1, i18n::text("sticker.recent")}};
                sticker_set_ids_ = {0, 0};
                applyStickerResponse(root, true);
            } else {
                int64_t cache_key = json_i53(extra, "index");
                ui_cache_.saveStickerItems(cache_key, root);
                applyStickerResponse(root, false);
            }
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra,"kind"),"reaction_icons")) {
        if (json_i53(extra,"chat")==current_chat_id_ && json_i53(extra,"message")==reaction_message_ && !std::strcmp(type,"stickers")) {
            size_t i;json_t* sticker;
            json_array_foreach(json_object_get(root,"stickers"),i,sticker) {
                const auto custom=json_i53(json_object_get(sticker,"full_type"),"custom_emoji_id");
                auto found=reaction_previews_.find(custom);
                if (found==reaction_previews_.end()) continue;
                auto* file=sticker_static_thumbnail(json_object_get(sticker,"thumbnail"));
                if (!file && !std::strcmp(json_str(json_object_get(sticker,"format"),"@type"),"stickerFormatWebp")) file=json_object_get(sticker,"sticker");
                for (auto id:media_cache_.enqueue(file)) pending_files_.erase(id);
                media_cache_.resolve(file,false);
                found->second.file=(int32_t)json_i53(file,"id");
                found->second.path=json_is_true(json_object_get(json_object_get(file,"local"),"is_downloading_completed")) ? json_str(json_object_get(file,"local"),"path") : "";
            }
        }
        retire_td_json(root,wire_bytes);return;
    }
    if (!std::strncmp(json_str(extra,"kind"),"post_",5)) {
        if (json_i53(extra,"chat")==current_chat_id_ && json_i53(extra,"generation")==history_generation_ && !std::strcmp(type,"message")) storeMessage(root);
        retire_td_json(root,wire_bytes);return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "reaction_choices") || !std::strcmp(json_str(extra, "kind"), "reaction_send")) {
        if (json_i53(extra, "chat") == current_chat_id_ && json_i53(extra, "message") == reaction_message_) {
            reaction_pending_ = false;
            if (!std::strcmp(json_str(extra, "kind"), "reaction_choices")) {
                reaction_loading_ = false; reaction_loaded_ = std::strcmp(type, "error") != 0;
                if (reaction_requested_at_) performance::observe(performance::Metric::ReactionResponse, performance::now()-reaction_requested_at_);
                reaction_requested_at_ = 0;
            }
            if (!std::strcmp(type, "error")) reaction_status_ = json_str(root, "message");
            else if (!std::strcmp(type, "availableReactions")) {
                reaction_choices_.clear(); reaction_custom_choices_.clear();
                auto add_custom=[&](int64_t id) {
                    if (id<=0) return;
                    for (const auto& entry:reaction_custom_choices_) if (entry.second==id) return;
                    std::string label=i18n::text("reaction.emoji_n", {std::to_string(reaction_custom_choices_.size()+1)});
                    reaction_custom_choices_[label]=id;reaction_choices_.push_back(label);
                };
                bool premium_available=false, paid_available=false;
                for (const char* field : {"top_reactions", "recent_reactions", "popular_reactions"}) {
                    size_t i; json_t* option;
                    json_array_foreach(json_object_get(root, field), i, option) {
                        json_t* kind = json_object_get(option, "type");
                        if (json_is_true(json_object_get(option,"needs_premium"))) { premium_available=true;continue; }
                        if (!std::strcmp(json_str(kind,"@type"),"reactionTypePaid")) { paid_available=true;continue; }
                        if (!std::strcmp(json_str(kind,"@type"),"reactionTypeCustomEmoji")) { add_custom(json_i53(kind,"custom_emoji_id"));continue; }
                        if (std::strcmp(json_str(kind,"@type"),"reactionTypeEmoji")) continue;
                        std::string emoji = json_str(kind, "emoji");
                        if (!emoji.empty() && std::find(reaction_choices_.begin(), reaction_choices_.end(), emoji) == reaction_choices_.end()) reaction_choices_.push_back(emoji);
                    }
                }
                for (const auto& message : messages_) if (message.id == reaction_message_)
                    for (const auto& reaction : message.reactions) {
                        if (reaction.chosen && reaction.custom_emoji_id) add_custom(reaction.custom_emoji_id);
                        if (reaction.chosen && reaction.emoji != "Stars" && reaction.emoji != "Custom" &&
                            std::find(reaction_choices_.begin(), reaction_choices_.end(), reaction.emoji) == reaction_choices_.end())
                            reaction_choices_.push_back(reaction.emoji);
                    }
                reaction_status_ = reaction_choices_.empty() ? i18n::text("reaction.none") : i18n::text("reaction.hint");
                if (reaction_choices_.empty() && premium_available) reaction_status_=i18n::text("reaction.premium");
                if (reaction_choices_.empty() && paid_available) reaction_status_=i18n::text("reaction.paid");
                const char* reason=json_str(json_object_get(root,"unavailability_reason"),"@type");
                if (!std::strcmp(reason,"reactionUnavailabilityReasonAnonymousAdministrator")) reaction_status_=i18n::text("reaction.anon_admin");
                else if (!std::strcmp(reason,"reactionUnavailabilityReasonGuest")) reaction_status_=i18n::text("reaction.guest");
                else if (!std::strcmp(reason,"reactionUnavailabilityReasonRestricted")) reaction_status_=i18n::text("reaction.restricted");
            } else {
                reaction_status_ = i18n::text("reaction.updated");
                sendJson(json_pack("{s:s,s:I,s:I}", "@type", "getMessage", "chat_id", (json_int_t)current_chat_id_, "message_id", (json_int_t)reaction_message_));
            }
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "voice_download") && !std::strcmp(type, "error")) {
        int32_t file = (int32_t)json_i53(extra, "file");
        if (file == active_voice_file_ && json_i53(extra, "generation") == voice_download_generation_) {
            pending_files_.erase(file);
            for (auto& message : history_items_) if (message.voice_file_id == file)
                message.voice_error = std::string("Audio download: ") + json_str(root, "message");
            rebuildHistory();
        }
        retire_td_json(root,wire_bytes); return;
    }
    const char* list_kind = json_is_object(extra) ? json_str(extra, "kind") : json_str(root, "@extra");
    if (!std::strcmp(list_kind, "avatar_metadata") && !std::strcmp(type, "error")) {
        // Retry only while the row is visible, with the existing 10s backoff.
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "animation_download") && !std::strcmp(type, "error")) {
        int32_t file = (int32_t)json_i53(extra, "file");
        if (file == active_animation_file_ && json_i53(extra, "generation") == animation_download_generation_) {
            pending_files_.erase(file);
            for (auto& message : history_items_) if (message.animation_file_id == file && message.animation_path.empty())
                message.animation_error = std::string("Media download: ") + json_str(root, "message");
            rebuildHistory();
        }
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(list_kind, "refresh_main_ids")) {
        if (!std::strcmp(type, "error")) {
            chats_error_ = json_str(root, "message");
            refresh_pending_ = false;
            notice_ = i18n::text("notice.refresh_failed"); notice_until_ = sceKernelGetSystemTimeWide() + 8000000;
        } else {
            json_t* ids = json_object_get(root, "chat_ids");
            size_t index; json_t* id;
            if (json_is_array(ids)) json_array_foreach(ids, index, id) {
                int64_t chat_id = json_integer_value(id);
                auto known = std::find_if(known_chats_.begin(), known_chats_.end(),
                    [&](const ChatItem& chat) { return chat.id == chat_id; });
                // Live TDLib updates already maintain known chat metadata. Refresh
                // cached snapshots once; refresh all entries on explicit user request.
                if ((refresh_manual_ || known == known_chats_.end() || known->cached_metadata) && refresh_ids_.insert(chat_id).second) {
                    sendListRequest("getChat", "refresh_main_chat", chat_id);
                }
            }
            if (refresh_ids_.empty()) finishChatRefresh();
        }
        retire_td_json(root,wire_bytes); return;
    }
    bool refresh_chat = !std::strcmp(list_kind, "refresh_main_chat");
    if (refresh_chat && !std::strcmp(type, "error")) {
        chats_error_ = json_str(root, "message");
        refresh_ids_.clear(); refresh_pending_ = false;
        notice_ = i18n::text("notice.refresh_failed"); notice_until_ = sceKernelGetSystemTimeWide() + 8000000;
        retire_td_json(root,wire_bytes); return;
    }
    if (!std::strcmp(list_kind, "load_main_chats")) {
        chats_pending_ = false;
        chats_request_deadline_ = 0;
        next_chats_request_ = sceKernelGetSystemTimeWide() + 500000;
        if (!std::strcmp(type, "error")) {
            if (json_i53(root, "code") == 404) chats_end_ = true;
            else chats_error_ = json_str(root, "message");
        }
        if (!std::strcmp(type, "ok") || (!std::strcmp(type, "error") && json_i53(root, "code") == 404)) {
            finishStartupChatList();
            chats_last_updated_[selected_folder_] = std::time(nullptr);
            // Telegram answered before the startup index was read: show the live list now.
            if (startup_list_hold_) { startup_list_hold_ = false; publishChats(); }
        }
        retire_td_json(root,wire_bytes);
        return;
    }
    bool history_reply = json_is_object(extra) && !std::strcmp(json_str(extra, "kind"), "history");
    if (history_reply && (json_i53(extra, "chat") != current_chat_id_ ||
        json_i53(extra, "generation") != history_generation_)) { retire_td_json(root,wire_bytes); return; }
    if (history_reply && !std::strcmp(type, "error")) {
        history_pending_ = false;
        history_error_ = json_str(root, "message");
    }
    if (!std::strcmp(type, "updateChatFolders")) {
        live_folders_received_ = true;
        parseFolders(root);
    } else if (!std::strcmp(type, "updateAuthorizationState") ||
        !std::strncmp(type, "authorizationState", 18)) {
        processAuth(root);
    } else if (!std::strcmp(type, "error")) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Telegram error %lld: %s", (long long)json_i53(root, "code"), json_str(root, "message"));
        error_ = buf;
        status_ = error_;
        if (!std::strcmp(json_str(json_object_get(root, "@extra"), "kind"), "media_upload")) {
            notice_ = error_; notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
            logUploads(true);
        }
        if (!std::strcmp(json_str(root, "@extra"), "send_message")) {
            notice_ = error_; notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
        }
    } else if (!std::strcmp(type, "message")) {
        storeMessage(root);
    } else if (!std::strcmp(type, "updateMessageSendSucceeded") || !std::strcmp(type, "updateMessageSendFailed")) {
        json_t* message = json_object_get(root, "message");
        storeMessage(message, json_i53(root, "old_message_id"));
        logUploads(true);
        if (!std::strcmp(type, "updateMessageSendFailed") && json_i53(message, "chat_id") == current_chat_id_) {
            notice_ = i18n::text("notice.send_failed"); notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
        }
    } else if (!std::strcmp(type, "updateChatReadOutbox")) {
        updateReadOutbox(json_i53(root, "chat_id"), json_i53(root, "last_read_outbox_message_id"));
    } else if (!std::strcmp(type, "chat")) {
        parseChat(root);
        if (refresh_chat) {
            refresh_ids_.erase(json_i53(root, "id"));
            if (refresh_pending_ && refresh_ids_.empty()) finishChatRefresh();
        }
    } else if (!std::strcmp(type, "chats")) {
        json_t* ids = json_object_get(root, "chat_ids");
        if (json_is_array(ids)) {
            size_t i; json_t* v;
            json_array_foreach(ids, i, v) {
                int64_t id = json_is_integer(v) ? json_integer_value(v) : 0;
                json_t* req = json_pack("{s:s,s:I}", "@type", "getChat", "chat_id", (json_int_t)id);
                sendJson(req);
            }
        }
    } else if (!std::strcmp(type, "messages")) {
        json_t* arr = json_object_get(root, "messages");
        if (history_reply && json_is_array(arr)) {
            // TDLib can omit interaction_info for older history entries. Keep
            // the cached reaction snapshot until an explicit update arrives.
            if (history_cached_) {
                for (const auto& item : history_items_)
                    if (!item.reactions.empty()) cached_reactions_.emplace(item.id, item.reactions);
                history_items_.clear(); history_cached_ = false;
            }
            history_pending_ = false;
            if (json_array_size(arr)) chat_live_ready_=true;
            int64_t old_cursor = history_cursor_;
            size_t i; json_t* m;
            json_array_foreach(arr, i, m) {
                if (json_is_object(m) && ((history_reply && current_thread_) || matchesTopic(m))) {
                    MessageItem parsed = parseMessage(m);
                    if (!json_is_object(json_object_get(json_object_get(m, "interaction_info"), "reactions"))) {
                        auto known = cached_reactions_.find(parsed.id);
                        if (known != cached_reactions_.end()) parsed.reactions = known->second;
                    }
                    if (replaced_message_ids_.count(parsed.id)) continue;
                    if (!history_cursor_ || parsed.id < history_cursor_) history_cursor_ = parsed.id;
                    if (std::none_of(history_items_.begin(), history_items_.end(), [&](const MessageItem& v) { return v.id == parsed.id; }))
                        history_items_.push_back(std::move(parsed));
                }
            }
            history_end_ = json_array_size(arr) == 0;
            if (!history_end_ && old_cursor && old_cursor == history_cursor_)
                history_error_ = i18n::text("history.no_older");
            history_done_ = history_end_ || (int)history_items_.size() >= history_target_;
            rebuildHistory();
            probeComments();
            status_ = history_done_ ? i18n::text("status.online") : i18n::text("history.loading_older");
        }
    } else if (!std::strcmp(type, "updateSupergroup")) {
        json_t* group = json_object_get(root, "supergroup");
        if (json_i53(group, "id")) {
            supergroup_forum_[json_i53(group, "id")] = json_is_true(json_object_get(group, "is_forum"));
            supergroup_member_[json_i53(group, "id")] = membershipOf(json_object_get(group, "status"));
            supergroup_rights_[json_i53(group, "id")] = postRightsOf(json_object_get(group, "status"));
        }
    } else if (!std::strcmp(type, "updateBasicGroup")) {
        json_t* group = json_object_get(root, "basic_group");
        if (json_i53(group, "id")) {
            basic_group_member_[json_i53(group, "id")] = membershipOf(json_object_get(group, "status"));
            basic_group_rights_[json_i53(group, "id")] = postRightsOf(json_object_get(group, "status"));
        }
    } else if (!std::strcmp(type, "updateChatPermissions")) {
        const int64_t id = json_i53(root, "chat_id");
        for (auto& chat : known_chats_) if (chat.id == id) {
            chat.can_send = json_is_true(json_object_get(json_object_get(root, "permissions"), "can_send_basic_messages"));
            break;
        }
    } else if (!std::strcmp(type, "updateNewChat")) {
        json_t* chat = json_object_get(root, "chat");
        if (json_is_object(chat)) parseChat(chat);
    } else if (!std::strcmp(type, "updateChatLastMessage") || !std::strcmp(type, "updateChatDraftMessage")) {
        int64_t id = json_i53(root, "chat_id");
        updateChatPositions(id, json_object_get(root, "positions"));
        noteLastMessage(id, json_object_get(root, "last_message"));
    } else if (!std::strcmp(type, "updateChatReadInbox") || !std::strcmp(type, "updateChatIsMarkedAsUnread")) {
        const int64_t id = json_i53(root, "chat_id");
        const bool read_inbox = !std::strcmp(type, "updateChatReadInbox");
        auto apply = [&](ChatItem& c) {
            if (c.id != id) return;
            if (read_inbox) c.unread_count = (int)json_i53(root, "unread_count");
            else c.marked_unread = json_is_true(json_object_get(root, "is_marked_as_unread"));
        };
        for (auto& c : known_chats_) apply(c);
        for (auto& c : chats_) apply(c);
    } else if (!std::strcmp(type, "updateChatPhoto")) {
        int64_t id = json_i53(root, "chat_id");
        json_t* small = json_object_get(json_object_get(root, "photo"), "small");
        json_t* local = json_object_get(small, "local");
        for (auto& chat : known_chats_) if (chat.id == id) {
            chat.avatar_file_id = (int32_t)json_i53(small, "id");
            chat.avatar_path = json_is_true(json_object_get(local, "is_downloading_completed")) ? json_str(local, "path") : "";
            // A null photo means the icon was removed, not a failed download.
        }
    } else if (!std::strcmp(type, "updateChatTitle")) {
        int64_t id = json_i53(root, "chat_id");
        for (auto& c : known_chats_) if (c.id == id) c.title = json_str(root, "title", c.title.c_str());
    } else if (!std::strcmp(type, "updateChatPosition")) {
        int64_t id = json_i53(root, "chat_id");
        json_t* position = json_object_get(root, "position");
        json_t* list = json_is_object(position) ? json_object_get(position, "list") : nullptr;
        int32_t list_id = chat_list_id(list);
        if (list_id == -1) { retire_td_json(root,wire_bytes); return; }
        auto found = std::find_if(known_chats_.begin(), known_chats_.end(), [&](const ChatItem& c) { return c.id == id; });
        if (found == known_chats_.end() && json_i53(position, "order")) {
            json_t* request = json_pack("{s:s,s:I}", "@type", "getChat", "chat_id", (json_int_t)id);
            sendJson(request);
        } else if (found != known_chats_.end()) {
            if (json_i53(position, "order")) found->list_orders[list_id] = json_i53(position, "order");
            else found->list_orders.erase(list_id);
        }
    } else if (!std::strcmp(type, "message")) {
        storeMessage(root);
    } else if (!std::strcmp(type, "updateMessageInteractionInfo") || !std::strcmp(type,"updateMessageReactions")) {
        if (json_i53(root, "chat_id") == current_chat_id_) {
            for (auto& message : history_items_) if (message.id == json_i53(root, "message_id")) {
                parse_reactions(message, !std::strcmp(type,"updateMessageReactions") ? root : json_object_get(root, "interaction_info"));
                if (!std::strcmp(type,"updateMessageInteractionInfo")) parse_comments(message, json_object_get(root, "interaction_info"));
            }
            rebuildHistory();
        }
    } else if (!std::strcmp(type, "updateMessageEdited")) {
        if (json_i53(root, "chat_id") == current_chat_id_) {
            for (auto& item : history_items_) if (item.id == json_i53(root, "message_id"))
                item.edit_date = json_i53(root, "edit_date");
            rebuildHistory();
        }
    } else if (!std::strcmp(type, "updateMessageContent")) {
        if (json_i53(root, "chat_id") == current_chat_id_) {
            json_t* content = json_object_get(root, "new_content");
            const char* kind = json_str(content, "@type");
            json_t* note = !std::strcmp(kind, "messageVoiceNote") ? json_object_get(content, "voice_note") :
                !std::strcmp(kind, "messageVideoNote") ? json_object_get(content, "video_note") : nullptr;
            if (note) {
                for (auto& item : history_items_) if (item.id == json_i53(root, "message_id")) {
                    const bool was_pending = item.transcription_pending;
                    parse_transcription(item, note);
                    if (item.transcription_pending) setTranscriptionStatus(i18n::text("post.transcription_pending"), 60);
                    else if (!item.transcription_error.empty()) setTranscriptionStatus(i18n::text("post.transcription_failed", {item.transcription_error}), 12);
                    else if (item.transcription_complete)
                        setTranscriptionStatus(i18n::text(item.transcription.empty() ? "post.transcription_empty" : "post.transcription_done"), 8);
                    else if (was_pending) setTranscriptionStatus("", 0);
                }
                rebuildHistory();
            }
        }
    } else if (!std::strcmp(type, "updateDeleteMessages")) {
        if (json_i53(root, "chat_id") == current_chat_id_) {
            size_t index; json_t* id;
            json_array_foreach(json_object_get(root, "message_ids"), index, id) {
                const int64_t deleted = json_integer_value(id);
                history_items_.erase(std::remove_if(history_items_.begin(), history_items_.end(),
                    [&](const MessageItem& item) { return item.id == deleted; }), history_items_.end());
                delete_permissions_.erase(deleted);
                if (reply_target_ == deleted) reply_target_ = 0;
                if (forward_chat_ == current_chat_id_ && forward_message_ == deleted) cancelForward();
            }
            rebuildHistory();
        }
    } else if (!std::strcmp(type, "updateFile")) {
        json_t* file = json_object_get(root, "file");
        if (json_is_object(file)) updateFile(file, false);
    } else if (!std::strcmp(type, "file")) {
        updateFile(root, false);
    } else if (!std::strcmp(type, "updateChatActiveStories") ||
               !std::strcmp(type, "updateStory") ||
               !std::strcmp(type, "updateStoryDeleted")) {
        // Stories are intentionally outside VitaGram's lightweight chat UI.
    } else if (!std::strcmp(type, "updateNewMessage")) {
        json_t* msg = json_object_get(root, "message");
        if (json_is_object(msg)) noteLastMessage(json_i53(msg, "chat_id"), msg);
        if (json_is_object(msg) && json_i53(msg, "chat_id") == current_chat_id_) {
            storeMessage(msg);
        }
    }

    retire_td_json(root,wire_bytes);
}

void TelegramClient::pump(bool interactive) {
    performance::Scope measurement(performance::Metric::Pump);
    if (message_delete_pending_ && sceKernelGetSystemTimeWide() >= message_delete_deadline_) {
        message_delete_pending_ = false; message_delete_deadline_ = 0;
        notice_ = i18n::text("message.delete_failed", {i18n::text("message.delete_timeout")});
        notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
    }
    void* restored = nullptr;
    if (gifs_.expire(sceKernelGetSystemTimeWide())) {
        gif_status_ = i18n::text("gif.timeout");
        gif_diagnostics::record("timeout", gif_request_kind_.c_str(), gifs_.generation - 1, performance::now());
    }
    lock_queue(&queue_mutex_);
    std::swap(restored, restored_snapshot_);
    const bool index_read = startup_index_read_;
    unlock_queue(&queue_mutex_);
    if (restored) beginCacheApply(restored);
    if (startup_list_hold_ && index_read && (!applying_snapshot_ || cache_apply_stage_ > 1)) {
        startup_list_hold_ = false; publishChats();
    }
    if (proxy_check_deadline_ && sceKernelGetSystemTimeWide() >= proxy_check_deadline_) {
        proxy_check_deadline_ = 0; ++proxy_check_generation_;
        proxy_check_status_ = i18n::text("proxy.check.timeout");
    }
    if (logging_out_ && closed_) {
        if (!cleanup_started_ && cleanup_result_ == 0) {
            stopWorker(); // Includes the final cache flush and TDLib destruction.
            status_ = i18n::text("account.deleting");
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 256 * 1024);
            cleanup_started_ = pthread_create(&cleanup_thread_, &attr, cleanupEntry, this) == 0;
            pthread_attr_destroy(&attr);
            if (!cleanup_started_) cleanup_result_ = -1;
        }
        if (cleanup_result_ == 1) status_ = i18n::text("account.deleted");
        else if (cleanup_result_ == -1)
            error_ = status_ = i18n::text("account.delete_incomplete");
        return;
    }
    if (!worker_started_) {
        if (!interactive) { applyCacheSlice(); applyCachedDetails(); applyCachedAvatar(); }
        return;
    }
    int64_t pump_start = sceKernelGetSystemTimeWide();
    if (!interactive) logUploads();
    size_t queued=0;
    lock_queue(&queue_mutex_);queued=parsed_.size();unlock_queue(&queue_mutex_);
    const int budget=td_pump_budget_us(queued,interactive,pump_start<interaction_until_.load());
    const int limit=interactive ? 4 : budget>2000 ? 128 : 32;
    bool changed = false;
    for (int i = 0; i < limit; ++i) {
        void* msg = nullptr;TdEventMetadata selected;
        lock_queue(&queue_mutex_);
        const bool background_turn = (++event_turn_ % 4) == 0;
        const auto select_started=performance::now();
        msg = take_td_event(parsed_, interactive && !background_turn, background_turn, current_chat_id_, history_generation_,&selected);
        performance::observe(performance::Metric::EventSelect,performance::now()-select_started);
        if (msg) parsed_wire_bytes_ -= selected.bytes;
        sampleQueues();
        unlock_queue(&queue_mutex_);
        if (!msg) break;
        int64_t received = selected.received;
        if (received > 0) performance::observe(performance::Metric::EventWait, performance::now()-received);
        changed = changed || selected.changes_chats;
        const bool first_history=selected.first_history && selected.chat==current_chat_id_ && selected.generation==history_generation_;
        const auto requested_at=first_history ? selected.requested : 0;
        const auto parsed_at=first_history ? selected.parsed : 0;
        const auto apply_started=performance::now();
        if (parsed_at>0) performance::observe(performance::Metric::FirstHistoryUiWait,apply_started-parsed_at);
        process(msg);
        if (first_history) {
            const auto applied_at=performance::now();
            performance::observe(performance::Metric::FirstHistoryApply,applied_at-apply_started);
            if (requested_at>0) performance::observe(performance::Metric::FirstHistoryReady,applied_at-requested_at);
        }
        if (sceKernelGetSystemTimeWide() - pump_start >= budget) break;
    }
    // Apply only file fields, never replay messages, send results or upload
    // progress. Live events processed above replace/cancel stale file jobs.
    for (int i = 0; !interactive && i < 4 &&
         sceKernelGetSystemTimeWide() - pump_start < 2000; ++i) {
        auto* file = media_cache_.takeValidated();
        if (!file) break;
        updateFile(file);
        json_decref(file);
    }
    if (!interactive && !history_pending_ && !history_done_ && history_error_.empty()) requestHistoryPage();
    if (changed) publishChats();
    if (interactive) return;
    if (sceKernelGetSystemTimeWide() - pump_start < 2000) {
        applyCacheSlice(); applyCachedDetails(); applyCachedAvatar();
        if (sceKernelGetSystemTimeWide()-pump_start<2000) applyStickerSlice();
    }
    if (startup_list_dirty_ && sceKernelGetSystemTimeWide() >= next_startup_list_save_) queueStartupList();
    drain_retired_json(retired_json.size()>256 ? 8000 : 2500);
    recoverListRequests(sceKernelGetSystemTimeWide());
    if (ready_ && !current_chat_id_ && !chats_pending_ && !chats_end_ && chats_error_.empty() &&
        (chats_.size() < chats_target_ || startup_chat_snapshots_.count(selected_folder_)) &&
        sceKernelGetSystemTimeWide() >= next_chats_request_) requestChatsPage();
    if (ready_ && !current_chat_id_ && !refresh_pending_ && sceKernelGetSystemTimeWide() >= next_update_check_)
        checkChatUpdates(false);
}

void TelegramClient::finishStartupChatList() {
    auto snapshot = startup_chat_snapshots_.find(selected_folder_);
    if (snapshot == startup_chat_snapshots_.end()) return;
    const size_t visible = std::min(chats_target_, snapshot->second.size());
    size_t live = 0;
    for (const auto& chat : known_chats_)
        if (!chat.cached_metadata && chat.list_orders.count(selected_folder_)) ++live;
    // loadChats may return fewer entries than requested; keep the old rows
    // until the visible window is filled or TDLib confirms the end of the list.
    if (!chats_end_ && live < visible) return;
    startup_chat_snapshots_.erase(snapshot);
    // Cached rows not confirmed by this TDLib session must not linger forever.
    for (auto& chat : known_chats_)
        if (chat.cached_metadata) chat.list_orders.erase(selected_folder_);
    publishChats();
}

void TelegramClient::queueStartupList() {
    // Only a list Telegram confirmed in this session replaces the previous index.
    if (logging_out_ || !cache_restore_allowed_ || chats_last_updated_.empty() || known_chats_.empty()) return;
    startup_list_dirty_ = false;
    next_startup_list_save_ = sceKernelGetSystemTimeWide() + 60000000;
    auto list_json = [](int32_t id) {
        return id == kArchiveFolderId ? json_pack("{s:s}", "@type", "chatListArchive") :
            id ? json_pack("{s:s,s:i}", "@type", "chatListFolder", "chat_folder_id", id) : json_pack("{s:s}", "@type", "chatListMain");
    };
    // The index keeps only the top rows of each list; serializing every known chat here
    // (~1200) cost a 70-80 ms UI frame per save. Pick the same rows startup_copy keeps.
    std::unordered_map<int32_t, std::vector<std::pair<int64_t, size_t>>> ranked;
    for (size_t i = 0; i < known_chats_.size(); ++i)
        for (const auto& entry : known_chats_[i].list_orders)
            if (entry.second) ranked[entry.first].push_back({entry.second, i});
    std::vector<bool> keep(known_chats_.size(), false);
    for (auto& list : ranked) {
        auto& rows = list.second;
        const size_t kept = std::min(UiCache::kStartupRowsPerList, rows.size());
        std::partial_sort(rows.begin(), rows.begin() + kept, rows.end(), [&](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : known_chats_[a.second].id > known_chats_[b.second].id;
        });
        for (size_t i = 0; i < kept; ++i) keep[rows[i].second] = true;
    }
    auto* chats = json_object();
    for (size_t index = 0; index < known_chats_.size(); ++index) {
        if (!keep[index]) continue;
        const auto& chat = known_chats_[index];
        auto* positions = json_array();
        for (const auto& entry : chat.list_orders) if (entry.second)
            json_array_append_new(positions, json_pack("{s:o,s:I}", "list", list_json(entry.first), "order", (json_int_t)entry.second));
        json_t* type = chat.user_id ? json_pack("{s:s,s:I}", "@type", "chatTypePrivate", "user_id", (json_int_t)chat.user_id) :
            chat.supergroup_id ? json_pack("{s:s,s:I,s:b}", "@type", "chatTypeSupergroup", "supergroup_id", (json_int_t)chat.supergroup_id, "is_channel", chat.channel) :
            chat.basic_group_id ? json_pack("{s:s,s:I}", "@type", "chatTypeBasicGroup", "basic_group_id", (json_int_t)chat.basic_group_id) :
            json_pack("{s:s}", "@type", "chatTypePrivate");
        auto outbox = read_outbox_.find(chat.id);
        json_object_set_new(chats, std::to_string(chat.id).c_str(), json_pack("{s:s,s:I,s:s,s:o,s:o,s:{s:b},s:i,s:b,s:{s:I},s:I}",
            "@type", "chat", "id", (json_int_t)chat.id, "title", chat.title.c_str(), "type", type, "positions", positions,
            "permissions", "can_send_basic_messages", chat.can_send, "unread_count", chat.unread_count,
            "is_marked_as_unread", chat.marked_unread, "last_message", "id", (json_int_t)chat.last_message_id,
            "last_read_outbox_message_id", (json_int_t)(outbox == read_outbox_.end() ? 0 : outbox->second)));
        // The cache does not store updateFile, so the shown picture is the only record of a fresh download.
        if (!chat.avatar_path.empty())
            json_object_set_new(json_object_get(chats, std::to_string(chat.id).c_str()), "@avatar_path", json_string(chat.avatar_path.c_str()));
    }
    const auto generation = ui_cache_.generation();
    lock_queue(&queue_mutex_);
    if (pending_startup_list_) json_decref(static_cast<json_t*>(pending_startup_list_));
    pending_startup_list_ = chats; pending_startup_generation_ = generation;
    unlock_queue(&queue_mutex_);
}

void TelegramClient::writeStartupList(bool final) {
    // Storage only. Kept pending until the full cache (names, avatars) has been merged.
    if (!full_cache_loaded_) return;
    lock_queue(&queue_mutex_);
    auto* chats = static_cast<json_t*>(pending_startup_list_); pending_startup_list_ = nullptr;
    const auto generation = pending_startup_generation_;
    unlock_queue(&queue_mutex_);
    if (!chats) return;
    auto proceed = [](void* opaque) -> bool { return !static_cast<TelegramClient*>(opaque)->stopping_; };
    ui_cache_.saveStartupList(chats, generation, final ? nullptr : +proceed, this);
    json_decref(chats);
}

void TelegramClient::publishChats() {
    performance::Scope measurement(performance::Metric::PublishChats);
    auto snapshot = startup_chat_snapshots_.find(selected_folder_);
    if (snapshot != startup_chat_snapshots_.end()) {
        selected_chat_count_ = snapshot->second.size();
        const size_t visible = std::min(chats_target_, snapshot->second.size());
        chats_.assign(snapshot->second.begin(), snapshot->second.begin()+visible);
        // The folder is shown from the startup index at once; don't time it until Telegram confirms.
        if (folder_selected_at_ && visible) {
            if (!folder_first_seen_) performance::observe(performance::Metric::FolderFirstRow, performance::now() - folder_selected_at_);
            performance::observe(performance::Metric::FolderReady, performance::now() - folder_selected_at_);
            performance::observe(performance::Metric::FolderRequests, (int64_t)folder_requests_);
            folder_first_seen_ = true; folder_selected_at_ = 0;
        }
        // Keep the previous order, but show what Telegram already reported for these chats.
        for (auto& row : chats_) {
            auto live = std::find_if(known_chats_.begin(), known_chats_.end(), [&](const ChatItem& c) { return c.id == row.id; });
            if (live == known_chats_.end() || live->cached_metadata) continue;
            const auto order = row.order; auto avatar = std::move(row.avatar_path);
            row = *live; row.order = order;
            if (row.avatar_path.empty() && row.avatar_file_id && !avatar.empty()) { row.avatar_path = std::move(avatar); row.avatar_stale = true; }
        }
        return;
    }
    if (startup_list_hold_) {
        chats_.clear(); selected_chat_count_ = 0;
        return;
    }
    startup_list_dirty_ = true;
    // TDLib can send cached updates beyond loadChats.limit. Expose only the
    // requested window; never download avatars for the undisplayed remainder.
    struct RankedChat { const ChatItem* item; int64_t order; };
    std::vector<RankedChat> ranked;
    ranked.reserve(known_chats_.size());
    for (const auto& chat : known_chats_) {
        auto order = chat.list_orders.find(selected_folder_);
        if (order == chat.list_orders.end() || !order->second) continue;
        ranked.push_back({&chat, order->second});
    }
    const size_t visible = std::min(chats_target_, ranked.size());
    std::partial_sort(ranked.begin(), ranked.begin()+visible, ranked.end(), [](const RankedChat& a, const RankedChat& b) {
        return a.order != b.order ? a.order > b.order : a.item->id > b.item->id;
    });
    selected_chat_count_ = ranked.size();
    if (folder_selected_at_ && visible && !folder_first_seen_) {
        folder_first_seen_ = true;
        performance::observe(performance::Metric::FolderFirstRow, performance::now() - folder_selected_at_);
    }
    if (folder_selected_at_ && (visible >= std::min<size_t>(chats_target_, 8) || chats_end_)) {
        performance::observe(performance::Metric::FolderReady, performance::now() - folder_selected_at_);
        performance::observe(performance::Metric::FolderRequests, (int64_t)folder_requests_);
        folder_selected_at_ = 0;
    }
    chats_.clear(); chats_.reserve(visible);
    for (size_t i = 0; i < visible; ++i) {
        chats_.push_back(*ranked[i].item); chats_.back().order = ranked[i].order;
    }
}

void TelegramClient::sendListRequest(const char* method, const char* kind, int64_t chat_id) {
    if (folder_selected_at_ && !std::strcmp(kind, "load_main_chats")) ++folder_requests_;
    json_t* request = json_pack("{s:s}", "@type", method);
    if (chat_id) json_object_set_new(request, "chat_id", json_integer(chat_id));
    else {
        json_object_set_new(request, "chat_list", selected_folder_ == kArchiveFolderId ? json_pack("{s:s}", "@type", "chatListArchive") :
            selected_folder_ ? json_pack("{s:s,s:i}", "@type", "chatListFolder", "chat_folder_id", selected_folder_) :
            json_pack("{s:s}", "@type", "chatListMain"));
        json_object_set_new(request, "limit", json_integer(15));
    }
    json_object_set_new(request, "@extra", json_pack("{s:s,s:I}", "kind", kind,
        "list_generation", (json_int_t)list_generation_));
    if (!std::strncmp(kind, "refresh_main", 12))
        json_object_set_new(json_object_get(request, "@extra"), "refresh_generation", json_integer(refresh_generation_));
    sendJson(request);
}

void TelegramClient::selectFolder(int32_t id) {
    if (id == selected_folder_) return;
    if (id && std::none_of(folders_.begin(), folders_.end(), [&](const ChatFolderItem& f) { return f.id == id; })) return;
    folder_windows_[selected_folder_] = {chats_target_, chats_end_};
    selected_folder_ = id; ++list_generation_;
    folder_selected_at_ = performance::now();
    folder_first_seen_ = false; folder_requests_ = 0;
    auto saved = folder_windows_.find(id);
    chats_target_ = saved == folder_windows_.end() ? 15 : saved->second.target;
    chats_pending_ = false;
    chats_end_ = saved != folder_windows_.end() && saved->second.end;
    chats_request_deadline_ = refresh_deadline_ = 0;
    refresh_pending_ = refresh_manual_ = false; refresh_ids_.clear();
    chats_error_.clear(); notice_.clear(); notice_until_ = 0;
    next_chats_request_ = 0; next_update_check_ = 0;
    refresh_top_ = true;
    publishChats();
    if (startup_chat_snapshots_.count(id) || saved == folder_windows_.end() || chats_.size() < chats_target_) requestChatsPage();
    checkChatUpdates(false);
}

void TelegramClient::parseFolders(void* opaque) {
    json_t* update = static_cast<json_t*>(opaque);
    json_t* items = json_object_get(update, "chat_folders");
    if (!json_is_array(items)) return;
    folders_.clear();
    size_t i; json_t* item;
    json_array_foreach(items, i, item) {
        int32_t id = (int32_t)json_i53(item, "id");
        if (id <= 0) continue;
        json_t* name = json_object_get(item, "name");
        std::string title = json_str(json_object_get(name, "text"), "text");
        if (title.empty()) title = json_str(item, "title");
        folders_.push_back({id, title.empty() ? i18n::text("folder.default") : i18n::Text(title)});
    }
    if (!folders_.empty()) {
        int position = std::max(0, std::min((int)folders_.size(), (int)json_i53(update, "main_chat_list_position")));
        folders_.insert(folders_.begin() + position, {0, i18n::text("folder.all")});
    }
    syncArchiveFolder();
    if (selected_folder_ && std::none_of(folders_.begin(), folders_.end(), [&](const ChatFolderItem& f) { return f.id == selected_folder_; })) {
        // A folder may be removed remotely while its chat is open.
        selectFolder(0);
    }
}

void TelegramClient::refreshLanguage() {
    for (auto* text : {&status_, &error_, &document_status_, &proxy_status_, &proxy_check_status_,
            &video_note_status_, &topics_status_, &sticker_status_, &gif_status_, &reaction_status_,
            &auth_detail_, &notice_, &transcription_status_, &history_error_, &chats_error_, &auth_next_via_, &current_chat_title_})
        text->refresh();
    syncArchiveFolder();
    for (auto& folder : folders_) folder.title.refresh();
    for (auto& pack : sticker_packs_) pack.title.refresh();
    for (auto& request : poll_requests_) request.second.status.refresh();
    for (auto& reply : reply_cache_) reply.second.text.refresh();
    for (auto& item : history_items_) {
        item.refreshLanguage();
        if (item.service_message) item.text = serviceMessageText(item);
    }
    // Custom reaction labels are also keys in the selection lookup.
    std::unordered_map<std::string, int64_t> translated;
    for (auto& label : reaction_choices_) {
        auto custom = reaction_custom_choices_.find(label);
        if (custom == reaction_custom_choices_.end()) continue;
        const auto id = custom->second;
        label = tr("reaction.emoji_n", {std::to_string(translated.size() + 1)});
        translated[label] = id;
    }
    reaction_custom_choices_ = std::move(translated);
    rebuildHistory(false);
}

void TelegramClient::setShowArchive(bool show) {
    show_archive_ = show;
    syncArchiveFolder();
    if (!show && selected_folder_ == kArchiveFolderId) selectFolder(0);
}

// The Archive tab follows the TDLib folders; without folders it still needs an All chats tab.
void TelegramClient::syncArchiveFolder() {
    // Existing built-in tabs must follow the new catalog too. User folder names stay intact.
    for (auto& folder : folders_) if (folder.id == 0) folder.title = i18n::text("folder.all");
    folders_.erase(std::remove_if(folders_.begin(), folders_.end(),
        [](const ChatFolderItem& f) { return f.id == kArchiveFolderId; }), folders_.end());
    if (!show_archive_) {
        if (folders_.size() == 1 && !folders_[0].id) folders_.clear();
        return;
    }
    if (folders_.empty()) folders_.push_back({0, i18n::text("folder.all")});
    folders_.push_back({kArchiveFolderId, i18n::text("folder.archive")});
}

void TelegramClient::rebuildHistory(bool request_missing) {
    performance::Scope measurement(performance::Metric::RebuildHistory);
    std::sort(history_items_.begin(), history_items_.end(), [](const MessageItem& a, const MessageItem& b) { return a.id < b.id; });
    messages_.clear();
    for (const auto& item : history_items_) {
        // Until mixed-media album navigation exists, keep each video accessible.
        // Only photo messages can safely be collapsed into the photo grid.
        if (item.media_album_id && !item.photos.empty() && !messages_.empty() &&
            !messages_.back().photos.empty() && messages_.back().media_album_id == item.media_album_id) {
            auto& album = messages_.back();
            album.edit_date = std::max(album.edit_date, item.edit_date);
            if (item.delivery == DeliveryState::Failed || album.delivery == DeliveryState::Failed) {
                album.delivery = DeliveryState::Failed;
                if (!item.send_error.empty()) album.send_error = item.send_error;
            } else if (item.delivery == DeliveryState::Pending || album.delivery == DeliveryState::Pending)
                album.delivery = DeliveryState::Pending;
            else if (item.delivery != DeliveryState::Read || album.delivery != DeliveryState::Read)
                album.delivery = DeliveryState::Sent;
            album.photos.insert(album.photos.end(), item.photos.begin(), item.photos.end());
            for (const auto& url : item.links)
                if (std::find(album.links.begin(), album.links.end(), url) == album.links.end()) album.links.push_back(url);
            if (album.text.empty() && !item.text.empty()) { album.text = item.text; album.link_spans = item.link_spans; }
            if (!album.has_comments && item.has_comments) {
                album.has_comments = true; album.comment_count = item.comment_count; album.comments_message_id = item.comments_message_id;
            }
            album.media_label = i18n::text("media.photos_count", {std::to_string(album.photos.size())});
        } else messages_.push_back(item);
    }
    resolveReplies(request_missing);
}

// Fills reply previews of the displayed messages: from loaded history first, then the lookup cache; an
// original that is in neither is requested once (at most a few at a time), never per frame.
void TelegramClient::resolveReplies(bool request_missing) {
    for (auto& message : messages_) {
        if (message.forwarded) {
            // Name of the original sender or channel; empty while unknown (the UI then shows a plain label).
            std::string source = message.forward_hidden_name;
            if (source.empty() && message.forward_from_chat) {
                for (const auto& chat : known_chats_) if (chat.id == message.forward_origin_id) { source = chat.title; break; }
            } else if (source.empty()) {
                auto user = user_names_.find(message.forward_origin_id);
                if (user != user_names_.end()) {
                    MessageItem sender; sender.sender_id = message.forward_origin_id;
                    source = senderName(sender);
                }
            }
            message.forward_source = source;
        }
        if (!message.reply_to_id) continue;
        const int64_t chat = message.reply_chat_id ? message.reply_chat_id : current_chat_id_;
        const std::pair<int64_t, int64_t> key{chat, message.reply_to_id};
        std::string sender; i18n::Text text = message.reply_text;
        bool found = false, missing = false;
        if (chat == current_chat_id_) {
            auto it = std::lower_bound(history_items_.begin(), history_items_.end(), message.reply_to_id,
                [](const MessageItem& item, int64_t id) { return item.id < id; });
            if (it != history_items_.end() && it->id == message.reply_to_id) { found = true; sender = senderName(*it); text = reply_preview_text(*it); }
        }
        if (!found) {
            auto cached = reply_cache_.find(key);
            if (cached != reply_cache_.end()) {
                found = true; missing = cached->second.missing; text = cached->second.text;
                if (!missing) { MessageItem who; who.sender_id = cached->second.sender_id; who.sender_chat = cached->second.sender_chat; sender = senderName(who); }
            }
        }
        if (request_missing && !found && text.empty() && message.reply_quote.empty() && ready_ && worker_started_ &&
            reply_requested_.size() < 6 && reply_requested_.insert(key).second) {
            auto* request = json_pack("{s:s,s:I,s:I}", "@type", "getMessage", "chat_id", (json_int_t)chat, "message_id", (json_int_t)message.reply_to_id);
            json_object_set_new(request, "@extra", json_pack("{s:s,s:I,s:I}", "kind", "reply_preview", "chat", (json_int_t)chat, "message", (json_int_t)message.reply_to_id));
            sendJson(request);
        }
        if (sender.empty()) sender = message.reply_origin_name;
        message.reply_sender = sender;
        message.reply_missing = missing;
        message.reply_text = missing ? i18n::text("reply.deleted") : !message.reply_quote.empty() ? i18n::Text(message.reply_quote) : text;
    }
}

void TelegramClient::refreshChats() {
    checkChatUpdates(true);
}

void TelegramClient::setTranscriptionStatus(const i18n::Text& text, int seconds) {
    transcription_status_ = text;
    transcription_status_until_ = sceKernelGetSystemTimeWide() + (int64_t)seconds * 1000000;
}
std::string TelegramClient::transcriptionStatus() const {
    return sceKernelGetSystemTimeWide() < transcription_status_until_ ? transcription_status_ : "";
}

std::string TelegramClient::updateNotice() const {
    return sceKernelGetSystemTimeWide() < notice_until_ ? notice_ : "";
}

void TelegramClient::checkChatUpdates(bool manual) {
    if (!ready_) return;
    recoverListRequests(sceKernelGetSystemTimeWide());
    if (refresh_pending_) {
        if (manual) {
            refresh_manual_ = true;
            lock_queue(&queue_mutex_);
            for (auto& request : outgoing_)
                if (request.text.find("\"kind\":\"refresh_main") != std::string::npos) request.priority = 3;
            unlock_queue(&queue_mutex_);
            notice_ = i18n::text("notice.refreshing_chats");
            notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
        }
        return;
    }
    refresh_pending_ = true; refresh_manual_ = manual;
    ++refresh_generation_;
    refresh_deadline_ = sceKernelGetSystemTimeWide() + 30000000;
    refresh_before_.clear();
    for (const auto& chat : chats_) refresh_before_.push_back(chat.id);
    refresh_ids_.clear(); chats_error_.clear();
    next_update_check_ = sceKernelGetSystemTimeWide() + 10000000;
    if (manual) { notice_ = i18n::text("notice.refreshing_chats"); notice_until_ = sceKernelGetSystemTimeWide() + 10000000; }
    // Snapshot the latest top entries; loadChats advances the older-page cursor.
    sendListRequest("getChats", "refresh_main_ids");
    if ((chats_.empty() || startup_chat_snapshots_.count(selected_folder_)) && !chats_pending_ && !chats_end_) requestChatsPage();
}

void TelegramClient::finishChatRefresh() {
    refresh_pending_ = false;
    refresh_deadline_ = 0;
    publishChats();
    chats_last_updated_[selected_folder_] = std::time(nullptr);
    if (refresh_manual_) refresh_top_ = true;
    std::vector<int64_t> current;
    for (const auto& chat : chats_) current.push_back(chat.id);
    const std::string new_message_prefix = i18n::text("notice.new_message", {""});
    if ((refresh_manual_ || current != refresh_before_) &&
        (sceKernelGetSystemTimeWide() >= notice_until_ || notice_.compare(0, new_message_prefix.size(), new_message_prefix) != 0)) {
        notice_ = i18n::text("notice.chats_updated");
        notice_until_ = sceKernelGetSystemTimeWide() + 5000000;
    }
}

void TelegramClient::updateChatPositions(int64_t id, void* opaque) {
    json_t* positions = static_cast<json_t*>(opaque);
    if (!json_is_array(positions)) return;
    auto found = std::find_if(known_chats_.begin(), known_chats_.end(), [&](const ChatItem& c) { return c.id == id; });
    if (found == known_chats_.end()) { sendJson(json_pack("{s:s,s:I}", "@type", "getChat", "chat_id", (json_int_t)id)); return; }
    found->list_orders.clear();
    size_t i; json_t* position;
    json_array_foreach(positions, i, position) {
        int32_t key = chat_list_id(json_object_get(position, "list"));
        if (key != -1 && json_i53(position, "order")) found->list_orders[key] = json_i53(position, "order");
    }
}

void TelegramClient::noteLastMessage(int64_t id, void* opaque) {
    json_t* message = static_cast<json_t*>(opaque);
    if (!json_is_object(message)) return;
    for (auto& chat : known_chats_) if (chat.id == id) {
        int64_t latest = json_i53(message, "id");
        if (latest > chat.last_message_id) {
            if (chat.list_orders.count(selected_folder_) && chat.last_message_id && !json_is_true(json_object_get(message, "is_outgoing"))) {
                notice_ = i18n::text("notice.new_message", {chat.title});
                notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
            }
            chat.last_message_id = latest;
        }
        break;
    }
}

void TelegramClient::loadMoreChats() {
    if (!canBrowse() || chatsLoading() || chatsEnd()) return;
    chats_error_.clear();
    chats_target_ += 15;
    publishChats();
    if (chats_.size() < chats_target_) requestChatsPage();
}

void TelegramClient::requestChatsPage() {
    if (!ready_ || chats_pending_ || chats_end_) return;
    chats_pending_ = true;
    chats_request_deadline_ = sceKernelGetSystemTimeWide() + 30000000;
    sendListRequest("loadChats", "load_main_chats");
    status_ = i18n::text("chats.loading");
}

void TelegramClient::resetListRequests(int64_t now) {
    // Keep displayed chats and pagination; invalidate only in-flight requests.
    ++list_generation_;
    chats_pending_ = refresh_pending_ = false;
    chats_request_deadline_ = refresh_deadline_ = 0;
    refresh_ids_.clear(); chats_error_.clear();
    next_chats_request_ = next_update_check_ = now;
}

void TelegramClient::recoverListRequests(int64_t now) {
    if ((chats_pending_ && chats_request_deadline_ && now >= chats_request_deadline_) ||
        (refresh_pending_ && refresh_deadline_ && now >= refresh_deadline_))
        resetListRequests(now);
}

void TelegramClient::syncOpenedChat() {
    if (!ready_) { opened_chat_id_ = 0; return; }
    if (opened_chat_id_ == current_chat_id_) return;
    if (opened_chat_id_)
        sendJson(json_pack("{s:s,s:I}", "@type", "closeChat", "chat_id", (json_int_t)opened_chat_id_));
    const bool from_list = !opened_chat_id_;
    opened_chat_id_ = current_chat_id_;
    if (opened_chat_id_)
        sendJson(json_pack("{s:s,s:I}", "@type", "openChat", "chat_id", (json_int_t)opened_chat_id_));
    if (opened_chat_id_ && from_list) dropListAvatarDownloads();
}

void TelegramClient::dropListAvatarDownloads() {
    // The list is hidden: its avatars must not compete with the open chat.
    // Unsent requests are dropped; sent ones are cancelled only if TDLib has
    // not started them. The list requests them again when shown.
    std::unordered_set<int32_t> unsent;
    lock_queue(&queue_mutex_);
    for (auto it = outgoing_.begin(); it != outgoing_.end();) {
        if (it->file_id && list_avatar_files_.count(it->file_id)) { unsent.insert(it->file_id); it = outgoing_.erase(it); }
        else ++it;
    }
    sampleQueues();
    unlock_queue(&queue_mutex_);
    for (auto id : list_avatar_files_) {
        if (!pending_files_.erase(id)) continue;
        file_priorities_.erase(id);
        if (!unsent.count(id))
            sendJson(json_pack("{s:s,s:i,s:b}", "@type", "cancelDownloadFile", "file_id", id, "only_if_pending", 1));
    }
    list_avatar_files_.clear();
}

void TelegramClient::openChat(int64_t id, const i18n::Text& title) {
    delete_permissions_.clear(); message_delete_pending_ = false; message_delete_deadline_ = 0;
    current_thread_ = pending_thread_; pending_thread_ = 0;
    comments_return_ = pending_return_; pending_return_ = {};
    comment_probed_.clear();
    next_jump_post_ = true;
    current_topic_ = 0; topics_open_ = topics_pending_ = false; topics_.clear(); ++topics_generation_;
    forum_title_ = title;
    replaced_message_ids_.clear();
    viewed_message_=viewed_message_at_=0;
    chat_open_at_=performance::now();chat_present_pending_=chat_live_present_pending_=true;chat_live_ready_=false;
    current_chat_id_ = id;
    syncOpenedChat();
    current_chat_title_ = title;
    membership_pending_ = false;
    if (ready_) for (const auto& chat : known_chats_) if (chat.id == id) {
        const bool super = chat.supergroup_id != 0;
        const int64_t group = super ? chat.supergroup_id : chat.basic_group_id;
        if (group && !(super ? supergroup_member_ : basic_group_member_).count(group)) {
            auto* request = json_pack("{s:s,s:I}", "@type", super ? "getSupergroup" : "getBasicGroup",
                                      super ? "supergroup_id" : "basic_group_id", (json_int_t)group);
            json_object_set_new(request, "@extra", json_pack("{s:s}", "kind", "group_status"));
            sendJson(request);
        }
        break;
    }
    if (ready_) for (const auto& chat : known_chats_) if (chat.id == id && chat.user_id) {
        sendJson(json_pack("{s:s,s:I,s:s}", "@type", "getUser", "user_id", (json_int_t)chat.user_id, "@extra", "sender_lookup"));
        break;
    }
    // TDLib reports a channel post's reply_info only once it knows the channel's linked discussion, which comes
    // with the full info. Ask once per channel; the answer re-probes the posts already shown ("channel_full").
    if (ready_ && !current_thread_) for (const auto& chat : known_chats_) if (chat.id == id && chat.channel && chat.supergroup_id) {
        if (channel_full_requested_.insert(chat.supergroup_id).second) {
            auto* request = json_pack("{s:s,s:I}", "@type", "getSupergroupFullInfo", "supergroup_id", (json_int_t)chat.supergroup_id);
            json_object_set_new(request, "@extra", json_pack("{s:s,s:I}", "kind", "channel_full", "chat", (json_int_t)id));
            sendJson(request);
        }
        break;
    }
    if (ready_ && !current_thread_) for (const auto& chat : known_chats_) if (chat.id == id && chat.group && chat.supergroup_id) {
        auto known = supergroup_forum_.find(chat.supergroup_id);
        if (known != supergroup_forum_.end()) {
            if (!known->second) break; // Ordinary group: history without getSupergroup.
            showTopicsList(); loadTopics(true); return;
        }
        // Unknown kind: show cached history and request the first page now;
        // the forum check runs in parallel instead of in front of it.
        refreshHistory();
        topics_pending_ = true; topics_status_ = i18n::text("topics.checking");
        json_t* req = json_pack("{s:s,s:I}", "@type", "getSupergroup", "supergroup_id", (json_int_t)chat.supergroup_id);
        json_object_set_new(req, "@extra", json_pack("{s:s,s:I,s:i}", "kind", "forum_check", "chat", (json_int_t)id, "generation", (int)topics_generation_));
        json_object_set_new(json_object_get(req,"@extra"),"requested_at",json_integer(performance::now()));
        sendJson(req); return;
    }
    refreshHistory();
}

bool TelegramClient::openTelegramLink(const std::string& url) {
    if (!ready_) return false;
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    size_t start = 0;
    for (const char* prefix : {"https://t.me/", "http://t.me/", "https://telegram.me/", "http://telegram.me/"})
        if (lower.compare(0, std::strlen(prefix), prefix) == 0) { start = std::strlen(prefix); break; }
    if (!start) return false;
    size_t end = url.find_first_of("/?#", start);
    std::string username = url.substr(start, end == std::string::npos ? end : end - start);
    if (username.size() < 5 || username.size() > 32) return false;
    for (unsigned char c : username) if (!std::isalnum(c) && c != '_') return false;
    // Invite links and service endpoints need their own TDLib link workflows.
    if (username == "share" || username == "addstickers" || username == "joinchat" || username == "proxy" || username == "socks") return false;
    if (end != std::string::npos && url[end] == '/') {
        size_t next = url.find_first_of("/?#", end + 1);
        std::string segment = url.substr(end + 1, next == std::string::npos ? next : next - end - 1);
        if (!segment.empty() && !std::all_of(segment.begin(), segment.end(), [](unsigned char c) { return std::isdigit(c); })) return false;
    }
    ++link_generation_;
    auto* request = json_pack("{s:s,s:s}", "@type", "searchPublicChat", "username", username.c_str());
    json_object_set_new(request, "@extra", json_pack("{s:s,s:i}", "kind", "public_link", "generation", (int)link_generation_));
    sendJson(request);
    return true;
}

void TelegramClient::showTopicsList() {
    topics_open_ = true; topics_pending_ = false;
    ++history_generation_; history_pending_ = false; history_done_ = true; history_items_.clear(); messages_.clear();
}

bool TelegramClient::matchesTopic(void* opaque) const {
    if (topics_open_) return false;
    if (current_thread_) {
        // Comments view: a message belongs to the thread when TDLib says so, or when it is the root or answers
        // something already shown. Cached or live messages of other threads in the same group are left out.
        json_t* message = (json_t*)opaque;
        const int64_t id = json_i53(message, "id");
        if (id == current_thread_) return true;
        auto* topic = json_object_get(message, "topic_id");
        if (!std::strcmp(json_str(topic, "@type"), "messageTopicThread")) return json_i53(topic, "message_thread_id") == current_thread_;
        auto* reply = json_object_get(message, "reply_to");
        if (!std::strcmp(json_str(reply, "@type"), "messageReplyToMessage")) {
            const int64_t target = json_i53(reply, "message_id");
            if (target == current_thread_) return true;
            for (const auto& item : history_items_) if (item.id == target) return true;
        }
        return false;
    }
    return !current_topic_ || json_i53(json_object_get((json_t*)opaque, "topic_id"), "forum_topic_id") == current_topic_;
}

void TelegramClient::loadTopics(bool refresh) {
    if (!ready_ || !current_chat_id_ || topics_pending_) return;
    if (refresh) { ++topics_generation_; topics_.clear(); topics_date_ = topics_offset_ = 0; topics_message_ = 0; topics_end_ = false; }
    if (topics_end_) return;
    topics_open_ = topics_pending_ = true; topics_status_ = i18n::text("topics.loading");
    json_t* req = json_pack("{s:s,s:I,s:s,s:i,s:I,s:i,s:i}", "@type", "getForumTopics", "chat_id", (json_int_t)current_chat_id_, "query", "", "offset_date", topics_date_, "offset_message_id", (json_int_t)topics_message_, "offset_forum_topic_id", topics_offset_, "limit", 15);
    json_object_set_new(req, "@extra", json_pack("{s:s,s:I,s:i}", "kind", "forum_topics", "chat", (json_int_t)current_chat_id_, "generation", (int)topics_generation_));
    sendJson(req);
}

void TelegramClient::openTopic(size_t index) {
    if (index >= topics_.size()) return;
    chat_open_at_=performance::now();chat_present_pending_=chat_live_present_pending_=true;chat_live_ready_=false;
    current_topic_ = topics_[index].id; topics_open_ = false;
    current_chat_title_ = forum_title_ + " / " + topics_[index].title;
    refreshHistory();
}

void TelegramClient::backToTopics() {
    current_topic_ = 0; topics_open_ = true; current_chat_title_ = forum_title_;
    ++history_generation_; history_pending_ = false; history_done_ = true; history_items_.clear(); messages_.clear();
}

bool TelegramClient::openPinnedMessage(int64_t message_id) {
    if (!current_chat_id_ || message_id <= 0) return false;
    for (const auto& item : history_items_) if (item.id == message_id) {
        for (const auto& displayed : messages_) if (displayed.id == message_id ||
            (item.media_album_id && displayed.media_album_id == item.media_album_id)) {
            message_jump_ = displayed.id;
            message_jump_post_ = next_jump_post_; next_jump_post_ = true;
            message_jump_generation_ = history_generation_;
            return true;
        }
    }
    if (!ready_ || !worker_started_) return false;
    auto* request = json_pack("{s:s,s:I,s:I}", "@type", "getMessage", "chat_id", (json_int_t)current_chat_id_, "message_id", (json_int_t)message_id);
    json_object_set_new(request, "@extra", json_pack("{s:s,s:I,s:i}", "kind", "pin_target", "chat", (json_int_t)current_chat_id_, "generation", (int)history_generation_));
    sendJson(request);
    return true;
}

bool TelegramClient::openComments(int64_t message_id, bool from_post) {
    if (!ready_ || !worker_started_ || !current_chat_id_ || current_thread_ || topics_open_) return false;
    int64_t target = 0;
    for (const auto& item : messages_) if (item.id == message_id && item.has_comments) { target = item.comments_message_id; break; }
    if (!target) return false;
    notice_ = i18n::text("comments.opening"); notice_until_ = sceKernelGetSystemTimeWide() + 4000000;
    auto* request = json_pack("{s:s,s:I,s:I}", "@type", "getMessageThread", "chat_id", (json_int_t)current_chat_id_, "message_id", (json_int_t)target);
    json_object_set_new(request, "@extra", json_pack("{s:s,s:I,s:I,s:b,s:i}", "kind", "comments_open", "chat", (json_int_t)current_chat_id_,
        "message", (json_int_t)message_id, "post", from_post, "generation", (int)history_generation_));
    sendJson(request);
    return true;
}

// History pages often carry no reply_info for channel posts; TDLib fills it in once the server has been asked about
// the posts, which viewMessages does. Probe each shown post once, newest first, then read them back.
void TelegramClient::probeComments() {
    if (!ready_ || !worker_started_ || !current_chat_id_ || current_thread_ || topics_open_) return;
    json_t* ids = json_array();
    for (auto it = history_items_.rbegin(); it != history_items_.rend() && json_array_size(ids) < 20; ++it)
        if (it->channel_post && !it->has_comments && it->id > 0 && comment_probed_.insert(it->id).second) json_array_append_new(ids, json_integer(it->id));
    if (!json_array_size(ids)) { json_decref(ids); return; }
    auto* request = json_pack("{s:s,s:I,s:{s:s},s:b,s:o}", "@type", "viewMessages", "chat_id", (json_int_t)current_chat_id_,
        "source", "@type", "messageSourceChatHistory", "force_read", 0, "message_ids", ids);
    json_object_set_new(request, "@extra", json_pack("{s:s,s:I,s:I,s:O}", "kind", "comments_probe", "chat", (json_int_t)current_chat_id_,
        "generation", (json_int_t)history_generation_, "ids", json_object_get(request, "message_ids")));
    sendJson(request);
}

void TelegramClient::closeComments() {
    if (!current_thread_) return;
    const CommentsReturn back = comments_return_;
    leaveChat();
    if (!back.chat) return;
    openChat(back.chat, back.title);
    link_chat_opened_ = true;
    next_jump_post_ = back.post;
    openPinnedMessage(back.message);
}

bool TelegramClient::requestTranscription(int64_t message_id) {
    if (!ready_ || !worker_started_ || !current_chat_id_) return false;
    auto found = std::find_if(history_items_.begin(), history_items_.end(), [&](const MessageItem& item) { return item.id == message_id; });
    if (found == history_items_.end() || !((found->voice && !found->audio) || found->video_note) ||
        found->transcription_pending || found->transcription_complete || !found->transcription.empty()) return false;
    found->transcription_pending = true;
    setTranscriptionStatus(i18n::text("post.transcription_wait"), 30);
    rebuildHistory();
    auto* request = json_pack("{s:s,s:I,s:I}", "@type", "recognizeSpeech", "chat_id", (json_int_t)current_chat_id_, "message_id", (json_int_t)message_id);
    json_object_set_new(request, "@extra", json_pack("{s:s,s:I,s:I}", "kind", "speech_request", "chat", (json_int_t)current_chat_id_, "message", (json_int_t)message_id));
    sendJson(request);
    return true;
}

void TelegramClient::storeMessage(void* opaque, int64_t old_id) {
    json_t* message = static_cast<json_t*>(opaque);
    if (!json_is_object(message) || json_i53(message, "chat_id") != current_chat_id_ || !current_chat_id_) return;
    if (!matchesTopic(message)) return;
    MessageItem parsed = parseMessage(message);
    // A message without interaction_info has no new reaction snapshot. Keep
    // known counts until TDLib supplies the info or an explicit reactions update.
    if (!json_is_object(json_object_get(json_object_get(message,"interaction_info"),"reactions")))
        for (const auto& item:history_items_) if (item.id==parsed.id) { parsed.reactions=item.reactions;break; }
    if (!json_is_object(json_object_get(message,"interaction_info")))
        for (const auto& item:history_items_) if (item.id==parsed.id) {
            parsed.has_comments=item.has_comments; parsed.comment_count=item.comment_count; parsed.comments_message_id=item.comments_message_id;
            break;
        }
    if (old_id && old_id != parsed.id) {
        replaced_message_ids_.insert(old_id);
        history_items_.erase(std::remove_if(history_items_.begin(), history_items_.end(),
            [&](const MessageItem& item) { return item.id == old_id; }), history_items_.end());
    }
    if (replaced_message_ids_.count(parsed.id)) return;
    auto found = std::find_if(history_items_.begin(), history_items_.end(), [&](const MessageItem& item) { return item.id == parsed.id; });
    if (found == history_items_.end()) history_items_.push_back(std::move(parsed));
    else if (!(parsed.delivery == DeliveryState::Pending && found->delivery != DeliveryState::Pending)) *found = std::move(parsed);
    rebuildHistory();
}

void TelegramClient::updateReadOutbox(int64_t chat_id, int64_t last_id) {
    if (last_id <= read_outbox_[chat_id]) return;
    read_outbox_[chat_id] = last_id;
    if (chat_id != current_chat_id_) return;
    for (auto& message : history_items_)
        if (message.outgoing && message.delivery == DeliveryState::Sent && message.id > 0 && message.id <= last_id)
            message.delivery = DeliveryState::Read;
    rebuildHistory();
}

void TelegramClient::refreshHistory() {
    if (!current_chat_id_ || topics_open_) return;
    ++history_generation_;
    cached_reactions_.clear();
    history_items_.clear(); messages_.clear();
    history_preferred_ = current_chat_id_;
    // Only the first screen is copied here, on the UI thread; offline the full cached history
    // follows from Storage (requestCachedDetails). 60 messages took 150-775 ms to copy and check.
    json_t* cached = ui_cache_.history(current_chat_id_, 12);
    const bool cached_partial = !json_object_size(cached) || ui_cache_.historyPending(current_chat_id_);
    media_cache_.resolve(cached, false); clear_cached_file_ids(cached);
    const char* key; json_t* message;
    if (json_is_object(cached)) json_object_foreach(cached, key, message) { if (matchesTopic(message)) history_items_.push_back(parseMessage(message)); }
    if (cached) json_decref(cached);
    history_cached_ = !history_items_.empty();
    rebuildHistory();
    history_cursor_ = 0; history_target_ = 60;
    history_pending_ = history_end_ = false; history_done_ = !ready_;
    history_error_.clear();
    if (!ready_ || cached_partial) requestCachedDetails();
    requestHistoryPage();
}

void TelegramClient::loadOlder() {
    if (!ready_ || !current_chat_id_ || history_pending_ || history_end_) return;
    if (history_done_) history_target_ += 60;
    history_done_ = false;
    history_error_.clear();
}

void TelegramClient::requestHistoryPage() {
    if (!current_chat_id_ || !ready_ || history_pending_ || topics_open_) return;
    history_pending_ = true;
    json_t* req = json_pack("{s:s,s:I,s:I,s:i,s:i,s:b}",
        "@type", "getChatHistory",
        "chat_id", (json_int_t)current_chat_id_,
        // TDLib IDs contain bit fields. Never increment/decrement an ID.
        // Keep the boundary message; the receiver deduplicates by exact ID.
        "from_message_id", (json_int_t)history_cursor_,
        "offset", 0,
        "limit", history_cursor_ ? 10 : 5,
        "only_local", 0);
    json_t* extra = json_pack("{s:s,s:I,s:i}", "kind", "history", "chat", (json_int_t)current_chat_id_, "generation", (int)history_generation_);
    json_object_set_new(extra,"first",json_boolean(history_cursor_==0));
    if (!history_cursor_) json_object_set_new(extra,"requested_at",json_integer(performance::now()));
    if (current_thread_) {
        json_object_set_new(req, "@type", json_string("getMessageThreadHistory"));
        json_object_set_new(req, "message_id", json_integer(current_thread_));
        json_object_del(req, "only_local");
    } else if (current_topic_) {
        json_object_set_new(req, "@type", json_string("getForumTopicHistory"));
        json_object_set_new(req, "forum_topic_id", json_integer(current_topic_));
        json_object_del(req, "only_local");
    }
    json_object_set_new(req, "@extra", extra);
    sendJson(req);
    status_ = i18n::text("history.loading");
}

void TelegramClient::openStickers() {
    ++sticker_session_; sticker_items_pending_set_=0;
    sticker_selected_pack_=-1;sticker_selection_pending_=sticker_selection_loaded_=false;
    if (sticker_packs_.size() < 2) {
        sticker_packs_ = {{0, i18n::text("sticker.favorites")}, {1, i18n::text("sticker.recent")}};
        sticker_set_ids_ = {0, 0};
    }
    stickers_.clear();
    sticker_status_ = ready_ ? i18n::text("sticker.choose_set") : i18n::text("net.no_telegram");
    requestCachedDetails(-2);
    selectStickerPack(1);
    loadStickerPacks();
}

void TelegramClient::loadStickerPacks() {
    if (!ready_ || !current_chat_id_ || sticker_packs_fresh_) return;
    json_t* root = json_pack("{s:s,s:{s:s}}", "@type", "getInstalledStickerSets", "sticker_type", "@type", "stickerTypeRegular");
    json_object_set_new(root, "@extra", json_pack("{s:s,s:I,s:i}", "kind", "sticker_packs", "chat", (json_int_t)current_chat_id_, "session", (int)sticker_session_));
    sendJson(root);
}

void TelegramClient::ensureStickerPackIcon(size_t index) {
    if (index >= sticker_packs_.size()) return;
    auto& pack = sticker_packs_[index];
    if (!pack.icon_path.empty()) return;
    if (pack.icon_file_id > 0) { requestFile(pack.icon_file_id, 20); return; }
    // Missing covers stay placeholders; selecting a set loads its contents.

}

void TelegramClient::selectStickerPack(int index) {
    if (index < 0 || index >= (int)sticker_set_ids_.size()) return;
    if (index==sticker_selected_pack_ && (sticker_selection_pending_ || sticker_selection_loaded_)) return;
    sticker_selected_pack_=index;sticker_selection_pending_=ready_ && current_chat_id_;sticker_selection_loaded_=false;
    ++sticker_generation_; stickers_.clear();
    sticker_items_pending_set_=index>=2 ? sticker_set_ids_[index] : 0;
    sticker_status_ = ready_ ? i18n::text("sticker.loading") : i18n::text("net.no_telegram");
    json_t* cached = ui_cache_.stickerItems(index);
    if (cached) {
        media_cache_.resolve(cached, false); clear_cached_file_ids(cached);
        applyStickerResponse(cached, false); json_decref(cached);
    }
    requestCachedDetails(index);
    if (!ready_ || !current_chat_id_) return;
    json_t* root = json_pack("{s:s}", "@type", index == 0 ? "getFavoriteStickers" : index == 1 ? "getRecentStickers" : "getStickerSet");
    if (index == 1) json_object_set_new(root, "is_attached", json_false());
    if (index >= 2) json_object_set_new(root, "set_id", json_integer(sticker_set_ids_[index]));
    json_object_set_new(root, "@extra", json_pack("{s:s,s:I,s:i,s:i,s:i}", "kind", "sticker_items", "chat", (json_int_t)current_chat_id_, "session", (int)sticker_session_, "generation", (int)sticker_generation_, "index", index));
    sendJson(root);
}

void TelegramClient::ensureSticker(size_t index) {
    if (index < stickers_.size() && stickers_[index].path.empty()) requestFile(stickers_[index].preview, 32);
}

bool TelegramClient::sendSticker(size_t index) {
    if (!ready_ || !current_chat_id_ || topics_open_ || index >= stickers_.size()) return false;
    const auto& s = stickers_[index];
    if (s.file <= 0) return false; // Offline cache has no session-valid send ID.
    json_t* input = json_pack("{s:s,s:{s:s,s:i},s:i,s:i}", "@type", "inputSticker", "sticker", "@type", "inputFileId", "id", s.file, "width", s.width, "height", s.height);
    json_t* content = json_pack("{s:s,s:o,s:s}", "@type", "inputMessageSticker", "sticker", input, "emoji", s.emoji.c_str());
    json_t* root = json_pack("{s:s,s:I,s:o,s:s}", "@type", "sendMessage", "chat_id", (json_int_t)current_chat_id_, "input_message_content", content, "@extra", "send_message");
    json_object_set_new(root, "@extra", uploads_.beginSticker(s.file, current_chat_id_, sceKernelGetSystemTimeWide()));
    sendJson(root); status_ = i18n::text("sticker.sending");
    logUploads(true);
    return true;
}

void TelegramClient::closeGifs() {
    gifs_.reset(0, 0, 0, ""); gif_status_.clear(); gif_request_kind_.clear();
}

void TelegramClient::searchGifs(const std::string& query) {
    gifs_.reset(current_chat_id_, current_topic_, current_thread_, query);
    if (!canPostCurrent()) { gif_status_ = i18n::text("gif.unavailable"); return; }
    gifs_.pending = true; gif_status_ = i18n::text("gif.loading");
    if (query.empty()) {
        auto* request = json_pack("{s:s}", "@type", "getSavedAnimations");
        json_object_set_new(request, "@extra", gifs_.extra("gif_saved")); sendJson(request);
    } else if (gif_bot_) requestGifResults("");
    else {
        auto* request = json_pack("{s:s,s:s}", "@type", "getOption", "name", "animation_search_bot_username");
        json_object_set_new(request, "@extra", gifs_.extra("gif_option")); sendJson(request);
    }
}

void TelegramClient::requestGifResults(const std::string& offset) {
    gifs_.pending = true; gif_status_ = i18n::text("gif.loading");
    auto* request = json_pack("{s:s,s:I,s:I,s:s,s:s}", "@type", "getInlineQueryResults",
        "bot_user_id", (json_int_t)gif_bot_, "chat_id", (json_int_t)gifs_.chat,
        "query", gifs_.query.c_str(), "offset", offset.c_str());
    json_object_set_new(request, "@extra", gifs_.extra("gif_results", offset)); sendJson(request);
}

void TelegramClient::loadMoreGifs() {
    if (canPostCurrent() && gifsCurrent() && !gifs_.pending && !gifs_.end && !gifs_.query.empty() && !gifs_.next_offset.empty())
        requestGifResults(gifs_.next_offset);
}

void TelegramClient::ensureGif(size_t index) {
    if (!gifsCurrent() || index >= gifs_.items.size()) return;
    auto& item = gifs_.items[index];
    if (!item.preview_reported) {
        item.preview_reported = true;
        gif_diagnostics::preview("preview_available", item.preview_format.c_str(), gifs_.generation, index + 1, item.preview,
            item.preview_width, item.preview_height, item.preview_size,
            item.path.empty() ? item.preview > 0 ? "remote" : item.preview_format.empty() ? "missing" : "unsupported" : "local",
            performance::now(), !item.fallback.empty());
    }
    if (!item.path.empty()) return;
    const bool pending = pending_files_.count(item.preview);
    requestFile(item.preview, 32);
    if (!pending && pending_files_.count(item.preview))
        gif_diagnostics::preview("preview_request", item.preview_format.c_str(), gifs_.generation, index + 1, item.preview,
            item.preview_width, item.preview_height, item.preview_size, "queued", performance::now());
}

bool TelegramClient::sendGif(size_t index) {
    if (!canPostCurrent() || !gifsCurrent()) return false;
    auto* request = gifs_.sendRequest(index);
    if (!request) return false;
    if (reply_target_) {
        json_object_set_new(request, "reply_to", json_pack("{s:s,s:I}", "@type", "inputMessageReplyToMessage", "message_id", (json_int_t)reply_target_));
        reply_target_ = 0;
    }
    sendJson(request); status_ = i18n::text("gif.sending");
    return true;
}

void TelegramClient::requestCompose() {
    if (current_chat_id_ && ready_ && !topics_open_) setInput(InputKind::Message, i18n::text("input.compose"));
}

bool TelegramClient::sendVideoNoteFile(const std::string& path, int duration, int64_t chat, int32_t topic) {
    if (!ready_ || !chat || path.empty() || duration < 1 || duration > video_note::MaxSeconds) {
        video_note_status_ = i18n::text("video_note.send_failed");
        return false;
    }
    SceIoStat info{};
    if (sceIoGetstat(path.c_str(), &info) < 0 || info.st_size <= 0) {
        video_note_status_ = i18n::text("video_note.file_missing"); return false;
    }
    json_t* request = video_note_message(path, duration, chat, topic);
    json_object_set_new(request, "@extra", uploads_.begin(path, info.st_size, true, chat, sceKernelGetSystemTimeWide()));
    sendJson(request);
    video_note_status_.clear();
    logUploads(true);
    return true;
}

bool TelegramClient::sendAudioFile(const std::string& path, int duration, int64_t chat, int32_t topic) {
    if (!ready_ || !chat || path.empty()) return false;
    SceIoStat file_info{};
    if (sceIoGetstat(path.c_str(), &file_info) < 0 || file_info.st_size <= 0) return false;
    json_t* file = json_pack("{s:s,s:s}", "@type", "inputFileLocal", "path", path.c_str());
    json_t* voice = json_pack("{s:s,s:o,s:i,s:s}", "@type", "inputVoiceNote", "voice_note", file,
        "duration", std::max(1, duration), "waveform", "");
    json_t* content = json_pack("{s:s,s:o,s:o}", "@type", "inputMessageVoiceNote", "voice_note", voice,
        "caption", json_pack("{s:s,s:[]}", "@type", "formattedText", "entities"));
    json_t* root = json_pack("{s:s,s:I,s:o,s:s}", "@type", "sendMessage", "chat_id", (json_int_t)chat,
        "input_message_content", content, "@extra", "send_audio");
    json_object_set_new(root, "topic_id", topic ? json_pack("{s:s,s:i}", "@type", "messageTopicForum", "forum_topic_id", topic) : json_null());
    json_object_set_new(root, "@extra", uploads_.begin(path, file_info.st_size, false, chat, sceKernelGetSystemTimeWide()));
    sendJson(root);
    logUploads(true);
    return true;
}

// kind: 0 photo, 1 video, 2 file. The modern schema wraps the file in inputPhoto / inputVideo / inputDocument.
static json_t* local_file_content(const std::string& path, int kind, bool original, MediaUploads::Kind& upload_kind) {
    auto local = [&] { return json_pack("{s:s,s:s}", "@type", "inputFileLocal", "path", path.c_str()); };
    auto caption = [] { return json_pack("{s:s,s:[]}", "@type", "formattedText", "entities"); };
    upload_kind = MediaUploads::Kind::File;
    if (kind == 0 && !original) {
        json_t* photo = json_pack("{s:s,s:o,s:i,s:i}", "@type", "inputPhoto", "photo", local(), "width", 0, "height", 0);
        upload_kind = MediaUploads::Kind::Photo;
        return json_pack("{s:s,s:o,s:o}", "@type", "inputMessagePhoto", "photo", photo, "caption", caption());
    }
    if (kind == 1 && !original) {
        json_t* video = json_pack("{s:s,s:o,s:i,s:i,s:i,s:b}", "@type", "inputVideo", "video", local(),
            "duration", 0, "width", 0, "height", 0, "supports_streaming", 1);
        upload_kind = MediaUploads::Kind::Video;
        return json_pack("{s:s,s:o,s:o}", "@type", "inputMessageVideo", "video", video, "caption", caption());
    }
    json_t* document = json_pack("{s:s,s:o,s:b}", "@type", "inputDocument", "document", local(),
        "disable_content_type_detection", kind != 2);
    if (kind == 0) upload_kind = MediaUploads::Kind::Photo;
    else if (kind == 1) upload_kind = MediaUploads::Kind::Video;
    return json_pack("{s:s,s:o,s:o}", "@type", "inputMessageDocument", "document", document, "caption", caption());
}

static json_t* forum_topic_id(int32_t topic) {
    return topic ? json_pack("{s:s,s:i}", "@type", "messageTopicForum", "forum_topic_id", topic) : json_null();
}

bool TelegramClient::sendLocalFile(const std::string& path, int kind, bool original, int64_t chat, int32_t topic) {
    if (!ready_ || !chat || path.empty()) return false;
    SceIoStat file_info{};
    if (sceIoGetstat(path.c_str(), &file_info) < 0 || file_info.st_size <= 0) return false;
    MediaUploads::Kind upload_kind;
    json_t* content = local_file_content(path, kind, original, upload_kind);
    json_t* root = json_pack("{s:s,s:I,s:o}", "@type", "sendMessage", "chat_id", (json_int_t)chat,
        "input_message_content", content);
    json_object_set_new(root, "topic_id", forum_topic_id(topic));
    json_object_set_new(root, "@extra", uploads_.beginFile(path, file_info.st_size, upload_kind, chat, sceKernelGetSystemTimeWide()));
    sendJson(root);
    logUploads(true);
    return true;
}

// Photos and videos share one album message, documents form their own album; a lone file is a plain message.
bool TelegramClient::sendLocalFiles(const std::vector<std::pair<std::string, int>>& files, int64_t chat, int32_t topic) {
    if (!ready_ || !chat) return false;
    bool ok = true;
    for (bool media : {true, false}) {
        std::vector<std::pair<std::string, int>> group;
        for (const auto& file : files) if ((file.second != 2) == media) group.push_back(file);
        if (group.empty()) continue;
        if (group.size() == 1) { ok = sendLocalFile(group[0].first, group[0].second, !media, chat, topic) && ok; continue; }
        json_t* contents = json_array();
        json_t* tokens = json_array();
        const int64_t now = sceKernelGetSystemTimeWide();
        for (const auto& file : group) {
            SceIoStat info{};
            if (sceIoGetstat(file.first.c_str(), &info) < 0 || info.st_size <= 0) { ok = false; continue; }
            MediaUploads::Kind upload_kind;
            json_array_append_new(contents, local_file_content(file.first, file.second, !media, upload_kind));
            json_t* extra = uploads_.beginFile(file.first, info.st_size, upload_kind, chat, now);
            json_array_append_new(tokens, json_incref(json_object_get(extra, "token")));
            json_decref(extra);
        }
        if (!json_array_size(contents)) { json_decref(contents); json_decref(tokens); continue; }
        json_t* root = json_pack("{s:s,s:I,s:o}", "@type", "sendMessageAlbum", "chat_id", (json_int_t)chat,
            "input_message_contents", contents);
        json_object_set_new(root, "topic_id", forum_topic_id(topic));
        json_object_set_new(root, "@extra", json_pack("{s:s,s:o}", "kind", "media_upload_album", "tokens", tokens));
        sendJson(root);
    }
    logUploads(true);
    return ok;
}

bool TelegramClient::pollPending(int64_t message) const {
    auto it = poll_requests_.find({current_chat_id_, message});
    return it != poll_requests_.end() && it->second.pending;
}
std::string TelegramClient::pollStatus(int64_t message) const {
    if (!ready_) return i18n::text("net.none");
    auto it = poll_requests_.find({current_chat_id_, message});
    return it == poll_requests_.end() ? "" : it->second.status;
}
bool TelegramClient::answerPoll(int64_t message, const std::vector<int>& options) {
    if (!ready_ || !current_chat_id_ || pollPending(message)) return false;
    auto it = std::find_if(messages_.begin(), messages_.end(), [message](const MessageItem& m) { return m.id == message; });
    if (it == messages_.end() || !it->poll.validAnswer(options)) return false;
    json_t* req = json_pack("{s:s,s:I,s:I,s:[]}", "@type", "setPollAnswer", "chat_id", (json_int_t)current_chat_id_, "message_id", (json_int_t)message, "option_ids");
    for (int option : options) json_array_append_new(json_object_get(req, "option_ids"), json_integer(option));
    json_object_set_new(req, "@extra", json_pack("{s:s,s:I,s:I}", "kind", "poll_vote", "chat", (json_int_t)current_chat_id_, "message", (json_int_t)message));
    poll_requests_[{current_chat_id_, message}] = {true, i18n::text("poll.sending_vote")};
    sendJson(req); return true;
}

void TelegramClient::messagesPresented() {
    if (!current_chat_id_ || topics_open_ || messages_.empty() || !chat_open_at_) return;
    const auto elapsed=performance::now()-chat_open_at_;
    if (chat_present_pending_) { performance::observe(performance::Metric::ChatFirstPresent,elapsed);chat_present_pending_=false; }
    if (chat_live_present_pending_ && chat_live_ready_) { performance::observe(performance::Metric::ChatFirstLivePresent,elapsed);chat_live_present_pending_=false; }
}

void TelegramClient::viewMessage(int64_t id) {
    if (!ready_ || !current_chat_id_ || id<=0) return;
    const auto now=sceKernelGetSystemTimeWide();
    if (viewed_message_==id && now-viewed_message_at_<5000000) return;
    const bool opened=viewed_message_!=id;
    viewed_message_=id;viewed_message_at_=now;
    if (opened) loadReactions(id);
    auto* request=json_pack("{s:s,s:I,s:{s:s},s:b}","@type","viewMessages","chat_id",(json_int_t)current_chat_id_,"source","@type",current_thread_ ? "messageSourceMessageThreadHistory" : current_topic_ ? "messageSourceForumTopicHistory" : "messageSourceChatHistory","force_read",0);
    auto* ids=json_array();json_array_append_new(ids,json_integer(id));json_object_set_new(request,"message_ids",ids);
    json_object_set_new(request,"@extra",json_pack("{s:s,s:I,s:I}","kind","post_view","chat",(json_int_t)current_chat_id_,"generation",(json_int_t)history_generation_));
    sendJson(request);
    auto* refresh=json_pack("{s:s,s:I,s:I}","@type","getMessage","chat_id",(json_int_t)current_chat_id_,"message_id",(json_int_t)id);
    json_object_set_new(refresh,"@extra",json_pack("{s:s,s:I,s:I}","kind","post_refresh","chat",(json_int_t)current_chat_id_,"generation",(json_int_t)history_generation_));
    sendJson(refresh);
}

void TelegramClient::loadReactions(int64_t id) {
    // Prefetched when the message opens: the picker reuses an answer that is
    // already loaded or on its way instead of asking TDLib again.
    if (ready_ && id > 0 && id == reaction_message_ && (reaction_loading_ || reaction_loaded_)) {
        if (reaction_loaded_ && !reaction_choices_.empty()) reaction_status_ = i18n::text("reaction.hint");
        return;
    }
    reaction_message_ = id; reaction_choices_.clear(); reaction_custom_choices_.clear(); reaction_previews_.clear(); reaction_pending_ = false;
    reaction_loading_ = reaction_loaded_ = false;
    reaction_status_ = ready_ ? i18n::text("reaction.loading") : i18n::text("net.none");
    if (!ready_ || !current_chat_id_ || id <= 0) return;
    reaction_loading_ = true; reaction_requested_at_ = performance::now();
    json_t* request = json_pack("{s:s,s:I,s:I,s:i}", "@type", "getMessageAvailableReactions",
        "chat_id", (json_int_t)current_chat_id_, "message_id", (json_int_t)id, "row_size", 8);
    json_object_set_new(request, "@extra", json_pack("{s:s,s:I,s:I}", "kind", "reaction_choices", "chat", (json_int_t)current_chat_id_, "message", (json_int_t)id));
    sendJson(request);
}
void TelegramClient::ensureReactionMedia(size_t index) {
    if (!ready_ || index>=reaction_choices_.size()) return;
    auto custom=reaction_custom_choices_.find(reaction_choices_[index]);
    if (custom==reaction_custom_choices_.end()) return;
    auto found=reaction_previews_.find(custom->second);
    if (found!=reaction_previews_.end()) {
        if (found->second.path.empty()) requestFile(found->second.file,32);
        return;
    }
    auto* ids=json_array();
    const size_t begin=index/32*32;
    for (size_t i=begin;i<reaction_choices_.size() && i<begin+32;++i) {
        auto item=reaction_custom_choices_.find(reaction_choices_[i]);
        if (item!=reaction_custom_choices_.end() && !reaction_previews_.count(item->second)) {
            reaction_previews_[item->second]={};json_array_append_new(ids,json_integer(item->second));
        }
    }
    auto* request=json_pack("{s:s}","@type","getCustomEmojiStickers");
    json_object_set_new(request,"custom_emoji_ids",ids);
    json_object_set_new(request,"@extra",json_pack("{s:s,s:I,s:I}","kind","reaction_icons","chat",(json_int_t)current_chat_id_,"message",(json_int_t)reaction_message_));
    sendJson(request);
}
std::string TelegramClient::reactionIcon(size_t index) const {
    if (index>=reaction_choices_.size()) return {};
    auto custom=reaction_custom_choices_.find(reaction_choices_[index]);
    if (custom==reaction_custom_choices_.end()) return {};
    return customReactionIcon(custom->second);
}

void TelegramClient::ensureMessageReactionMedia() {
    if (!ready_ || !reaction_message_) return;
    auto* ids=json_array();
    for (const auto& message:messages_) if (message.id==reaction_message_)
        for (const auto& reaction:message.reactions) {
            if (!reaction.custom_emoji_id) continue;
            auto found=reaction_previews_.find(reaction.custom_emoji_id);
            if (found!=reaction_previews_.end()) {
                if (found->second.path.empty()) requestFile(found->second.file,32);
            } else if (json_array_size(ids)<100) {
                reaction_previews_[reaction.custom_emoji_id]={};
                json_array_append_new(ids,json_integer(reaction.custom_emoji_id));
            }
        }
    if (!json_array_size(ids)) { json_decref(ids);return; }
    auto* request=json_pack("{s:s}","@type","getCustomEmojiStickers");
    json_object_set_new(request,"custom_emoji_ids",ids);
    json_object_set_new(request,"@extra",json_pack("{s:s,s:I,s:I}","kind","reaction_icons","chat",(json_int_t)current_chat_id_,"message",(json_int_t)reaction_message_));
    sendJson(request);
}

std::string TelegramClient::customReactionIcon(int64_t custom_emoji_id) const {
    auto found=reaction_previews_.find(custom_emoji_id);
    return found==reaction_previews_.end() ? std::string{} : found->second.path;
}

void TelegramClient::toggleReaction(int64_t id, const std::string& emoji) {
    if (!ready_ || id != reaction_message_ || reaction_pending_) return;
    if (std::find(reaction_choices_.begin(),reaction_choices_.end(),emoji)==reaction_choices_.end()) return;
    const auto custom_it=reaction_custom_choices_.find(emoji);
    const int64_t custom=custom_it==reaction_custom_choices_.end() ? 0 : custom_it->second;
    bool chosen = false;
    for (const auto& message : messages_) if (message.id == id)
        for (const auto& reaction : message.reactions)
            if (custom ? reaction.custom_emoji_id==custom : !reaction.custom_emoji_id && reaction.emoji==emoji) chosen=reaction.chosen;
    json_t* request = json_pack("{s:s,s:I,s:I}", "@type", chosen ? "removeMessageReaction" : "addMessageReaction",
        "chat_id", (json_int_t)current_chat_id_, "message_id", (json_int_t)id);
    json_object_set_new(request,"reaction_type", custom ? json_pack("{s:s,s:I}","@type","reactionTypeCustomEmoji","custom_emoji_id",(json_int_t)custom) :
        json_pack("{s:s,s:s}","@type","reactionTypeEmoji","emoji",emoji.c_str()));
    if (!chosen) { json_object_set_new(request, "is_big", json_false()); json_object_set_new(request, "update_recent_reactions", json_true()); }
    json_object_set_new(request, "@extra", json_pack("{s:s,s:I,s:I}", "kind", "reaction_send", "chat", (json_int_t)current_chat_id_, "message", (json_int_t)id));
    reaction_pending_ = true; reaction_status_ = i18n::text("common.sending"); sendJson(request);
}

void TelegramClient::leaveChat() {
    delete_permissions_.clear(); message_delete_pending_ = false; message_delete_deadline_ = 0;
    closeGifs();
    reply_target_ = 0;
    current_thread_ = pending_thread_ = 0; comments_return_ = pending_return_ = {}; next_jump_post_ = true;
    current_topic_ = 0; topics_open_ = topics_pending_ = false; topics_.clear(); ++topics_generation_;
    reaction_message_ = 0; reaction_choices_.clear(); reaction_custom_choices_.clear(); reaction_previews_.clear(); reaction_pending_ = false;
    reaction_loading_ = reaction_loaded_ = false;
    ++history_generation_;
    history_pending_ = false;
    history_done_ = true;
    history_items_.clear();
    current_chat_id_ = 0;
    foreground_until_.store(0);
    syncOpenedChat();
    next_update_check_ = 0;
    current_chat_title_.clear();
    messages_.clear();
    input_kind_ = InputKind::None;
    status_ = i18n::text("status.online");
}

void TelegramClient::close() {
    if (worker_started_ && !closed_) sendRaw("{\"@type\":\"close\"}");
}

std::string TelegramClient::currentChatAvatar() const {
    for (const auto& chat : known_chats_) if (chat.id == current_chat_id_) return chat.avatar_path;
    return "";
}

void* TelegramClient::cleanupEntry(void* opaque) {
    auto* self = static_cast<TelegramClient*>(opaque);
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Parser);
    self->cleanup_result_ = account_cleanup::purge() ? 1 : -1;
    return nullptr;
}

void TelegramClient::clearAccountMemory() {
    document_downloads_.clear();
    document_status_.clear();
    cache_restore_allowed_ = false;
    ready_ = false; input_kind_ = InputKind::None;
    connection_ready_ = false; cancelProxyCheck();
    chats_request_deadline_ = refresh_deadline_ = 0;
    startup_chat_snapshots_.clear(); startup_list_hold_ = false;
    chats_last_updated_.clear();
    known_chats_.clear(); chats_.clear(); leaveChat();
    ui_cache_.clear(); media_cache_.clear();
    user_names_.clear(); user_photos_.clear(); sender_requests_.clear(); user_presence_.clear(); supergroup_forum_.clear(); supergroup_member_.clear(); basic_group_member_.clear(); clearSearch();
    folder_windows_.clear(); folders_.clear(); selected_folder_ = 0; syncArchiveFolder();
    stickers_.clear(); sticker_packs_.clear(); sticker_set_ids_.clear(); sticker_status_.clear();
    sticker_packs_fresh_ = false; sticker_icon_retry_.clear();
    ++sticker_generation_; ++sticker_session_; sticker_items_pending_set_=0;
    gif_bot_ = 0;
    pending_files_.clear(); media_streams_.clear(); file_priorities_.clear(); avatar_metadata_retry_.clear(); list_avatar_files_.clear();
    file_update_times_.clear();
    read_outbox_.clear(); replaced_message_ids_.clear(); poll_requests_.clear();
    refresh_before_.clear(); refresh_ids_.clear(); refresh_pending_ = refresh_manual_ = false;
    chats_pending_ = requested_chats_ = cache_maintenance_ = false;
    notice_.clear(); chats_error_.clear(); video_note_status_.clear();
    uploads_ = MediaUploads{};
    uploads_pending_ = false;
    upload_log_snapshot_.clear();
    lock_queue(&queue_mutex_);
    outgoing_.clear(); upload_logs_.clear();
    sampleQueues();
    unlock_queue(&queue_mutex_);
}

void TelegramClient::logout() {
    if (ready_) sendRaw("{\"@type\":\"logOut\"}");
}

void TelegramClient::optimizeCache(int days, int megabytes) {
    if (!ready_ || current_chat_id_ || cache_maintenance_) return;
    json_t* request = json_pack("{s:s,s:I,s:i,s:i,s:i,s:[],s:[],s:[],s:b,s:i}",
        "@type", "optimizeStorage", "size", megabytes ? (json_int_t)megabytes * 1024 * 1024 : ((json_int_t)1 << 52),
        "ttl", days ? days * 86400 : INT32_MAX, "count", INT32_MAX, "immunity_delay", 60,
        "file_types", "chat_ids", "exclude_chat_ids", "return_deleted_file_statistics", 0, "chat_limit", 0);
    json_t* types = json_object_get(request, "file_types");
    for (const char* type : {"fileTypeAnimation", "fileTypeAudio", "fileTypeDocument", "fileTypePhoto", "fileTypeVideo", "fileTypeVideoNote", "fileTypeVoiceNote", "fileTypeThumbnail", "fileTypeProfilePhoto", "fileTypeSticker"})
        json_array_append_new(types, json_pack("{s:s}", "@type", type));
    json_object_set_new(request, "@extra", json_string("cache_cleanup"));
    cache_maintenance_ = true;
    sendJson(request);
}
