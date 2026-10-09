#pragma once
#include "proxy_config.h"
#include "vault_sweep.h"
#include "media_uploads.h"
#include "td_event_queue.h"
#include "poll.h"
#include <map>
#include "media_cache.h"
#include "ui_cache.h"
#include "media_trash.h"
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_set>
#include <deque>
#include <pthread.h>
#include <atomic>
#include <algorithm>
#include <unordered_map>

struct ChatItem {
    int64_t supergroup_id = 0;
    int64_t user_id = 0;
    bool group = false;
    bool cached_metadata = false;
    std::unordered_map<int32_t, int64_t> list_orders; // 0 = main, >0 = folder
    int64_t id = 0;
    int64_t order = 0;
    int64_t last_message_id = 0;
    std::string title;
    int32_t avatar_file_id = 0;
    std::string avatar_path;
};

struct PhotoItem {
    int width = 0, height = 0;
    int32_t thumbnail_file_id = 0;
    std::string thumbnail_path;
    int32_t full_file_id = 0;
    std::string full_path;
};

enum class DeliveryState { Pending, Sent, Read, Failed };
struct ForumTopicItem { int32_t id = 0; std::string title; int unread = 0; bool closed = false; };
struct SendStickerItem { int32_t file = 0, preview = 0; int width = 0, height = 0; std::string emoji, path; };
struct ChatFolderItem {
    int32_t id = 0;
    std::string title;
    int32_t icon_file_id = 0;
    std::string icon_path;
};

struct MessageItem {
    PollData poll;
    struct Reaction { std::string emoji; int count = 0; bool chosen = false; int64_t custom_emoji_id = 0; };
    std::vector<Reaction> reactions;
    int64_t sender_id = 0;
    bool sender_chat = false;
    int64_t id = 0;
    int64_t date = 0, edit_date = 0;
    int64_t media_album_id = 0;
    bool outgoing = false;
    DeliveryState delivery = DeliveryState::Sent;
    std::string send_error;
    std::string text;
    std::string media_label;
    bool pin_message = false;
    bool service_message = false;
    bool member_removed = false;
    int64_t removed_member_id = 0;
    int64_t pinned_message_id = 0;
    int32_t media_file_id = 0;
    std::string media_path;
    bool document = false;
    std::string document_name;
    int64_t document_size = 0, document_downloaded = 0;
    bool sticker = false;
    bool animation = false;
    bool video = false, video_note = false; // Shares the animation download/preview fields.
    int video_duration = 0;
    int video_width = 0, video_height = 0;
    bool video_alternative = false;
    bool voice = false;
    bool audio = false; // Ordinary audio uses the same download/playback fields as voice.
    std::string audio_title, audio_performer;
    int voice_duration = 0;
    int32_t voice_file_id = 0;
    int64_t voice_size = 0, voice_downloaded = 0;
    std::string voice_path, voice_error;
    std::string transcription;
    bool transcription_pending = false;
    bool transcription_complete = false;
    int32_t animation_file_id = 0;
    int64_t animation_size = 0, animation_downloaded = 0;
    std::string animation_path, animation_error;
    std::vector<PhotoItem> photos;
    std::vector<std::string> links;
};

enum class InputKind {
    None,
    Phone,
    Code,
    Password,
    FirstName,
    Message
};

class TelegramClient {
public:
    TelegramClient();
    ~TelegramClient();

    bool start(bool defer_cache_worker = false);
    bool startCacheWorker(); // UI; also supports offline background restoration.
    void configureProxy(bool enabled, const ProxyConfig& proxy);
    void configureProxy(bool enabled, const std::string& server, int port, const std::string& secret) {
        ProxyConfig proxy; proxy.server = server; proxy.port = port; proxy.secret = secret;
        configureProxy(enabled, proxy);
    }
    const std::string& proxyStatus() const { return proxy_status_; }
    void checkProxy(const ProxyConfig& proxy);
    void checkProxy(const std::string& server, int port, const std::string& secret) {
        ProxyConfig proxy; proxy.server = server; proxy.port = port; proxy.secret = secret;
        checkProxy(proxy);
    }
    void cancelProxyCheck();
    // True when TDLib's database is protected with the key it should have now (vault key while the
    // vault is on, console key otherwise). False while a re-key is pending or running.
    bool databaseKeyCurrent() const;
    const std::string& proxyCheckStatus() const { return proxy_check_status_; }
    void setRequestBatchSize(int size) { request_batch_size_.store(std::max(1, std::min(64, size))); }
    void restoreCache(); // UI, before pump() and before the cache writer starts.
    bool restoreCacheAsync(); // Startup: keep UI/TDLib running during cache reads.
    bool canBrowse() const { return ready_ || !known_chats_.empty(); }
    void pump(bool interactive = false);
    void prioritizeInteraction(int64_t now) { interaction_until_.store(now + 150000); }
    void close();
    void logout();
    void optimizeCache(int days, int megabytes);
    bool cacheMaintenance() const { return cache_maintenance_; }
    std::string currentChatAvatar() const;
    void stopWorker();
    void loadOlder();
    int historyPercent() const { return history_done_ ? 100 : std::min(99, (int)history_items_.size() * 100 / history_target_); }
    bool historyLoading() const { return current_chat_id_ && !history_done_; }
    const std::string& historyError() const { return history_error_; }

    bool ready() const { return ready_; }
    bool closed() const { return closed_; }
    const std::string& status() const { return status_; }
    std::string uploadStatus() const;
    void dismissStatusNotices() {
        uploads_.dismissCompleted();
        notice_.clear(); notice_until_ = 0;
    }
    const std::string& lastError() const { return error_; }

    InputKind inputKind() const { return input_kind_; }
    std::string inputPrompt() const;
    bool inputSecret() const { return input_kind_ == InputKind::Password; }
    void submitInput(const std::string& value);
    // Sign-in helpers: where the code was sent, step back to the phone number, ask for a new code.
    const std::string& authDetail() const { return auth_detail_; }
    bool canResendCode() const;
    // "Next: SMS in 42 s" / "Press triangle to get it by SMS"; empty when no other method exists.
    std::string authResendInfo() const;
    bool canGoBackInput() const { return input_kind_ == InputKind::Code || input_kind_ == InputKind::Password || input_kind_ == InputKind::FirstName; }
    // QR sign-in: Telegram's tg://login link while waiting for the phone to confirm (empty otherwise).
    const std::string& qrLink() const { return qr_link_; }
    bool canRequestQr() const { return !ready_ && (input_kind_ == InputKind::Phone || input_kind_ == InputKind::Code || input_kind_ == InputKind::Password); }
    void requestQr();
    void backInput();
    void resendCode();
    void cancelInput();

    const std::vector<ChatItem>& chats() const { return chats_; }
    const std::vector<MessageItem>& messages() const { return messages_; }
    void downloadDocument(const MessageItem& message, const std::string& directory);
    const std::string& documentStatus() const { return document_status_; }
    int64_t currentChatId() const { return current_chat_id_; }
    const std::string& currentChatTitle() const { return current_chat_title_; }
    std::string currentUserStatus() const;
    bool currentChatIsGroup() const;
    std::string senderAvatar(const MessageItem& message) const;
    std::string senderName(const MessageItem& message) const;
    std::string serviceMessageText(const MessageItem& message) const;

    void refreshChats();
    const std::vector<ChatFolderItem>& folders() const { return folders_; }
    int32_t selectedFolder() const { return selected_folder_; }
    void selectFolder(int32_t id);
    std::string updateNotice() const;
    bool takeChatTopRequest() { bool result = refresh_top_; refresh_top_ = false; return result; }
    void loadMoreChats();
    bool chatsLoading() const { return ready_ && !chats_end_ && chats_error_.empty() && (chats_.size() < chats_target_ || startup_chat_snapshots_.count(selected_folder_)); }
    bool chatsEnd() const { return chats_end_ && chats_.size() == selected_chat_count_; }
    const std::string& chatsError() const { return chats_error_; }
    void openChat(int64_t chat_id, const std::string& title);
    bool openPinnedMessage(int64_t message_id);
    int64_t takeMessageJump() { int64_t id = message_jump_generation_ == history_generation_ ? message_jump_ : 0; message_jump_ = 0; return id; }
    bool openTelegramLink(const std::string& url);
    bool takeLinkChatOpened() { bool opened = link_chat_opened_; link_chat_opened_ = false; return opened; }
    bool topicsOpen() const { return topics_open_; }
    const std::vector<ForumTopicItem>& topics() const { return topics_; }
    const std::string& topicsStatus() const { return topics_status_; }
    void loadTopics(bool refresh = false);
    void openTopic(size_t index);
    void backToTopics();
    bool inTopic() const { return current_topic_ != 0; }
    void refreshHistory();
    void requestCompose();
    bool requestTranscription(int64_t message_id);
    bool sendVideoNoteFile(const std::string& path, int duration, int64_t chat, int32_t topic);
    const std::string& videoNoteStatus() const { return video_note_status_; }
    int32_t currentTopicId() const { return current_topic_; }
    bool sendAudioFile(const std::string& path, int duration, int64_t chat, int32_t topic);
    // kind: 0 photo, 1 video, 2 file. original=true sends a document without recompression.
    bool sendLocalFiles(const std::vector<std::pair<std::string, int>>& files, int64_t chat, int32_t topic);
    bool sendLocalFile(const std::string& path, int kind, bool original, int64_t chat, int32_t topic);
    void openStickers();
    void loadStickerPacks();
    void selectStickerPack(int index);
    void ensureStickerPackIcon(size_t index);
    void ensureSticker(size_t index);
    bool sendSticker(size_t index);
    const std::vector<SendStickerItem>& stickers() const { return stickers_; }
    const std::vector<ChatFolderItem>& stickerPacks() const { return sticker_packs_; }
    const std::string& stickerStatus() const { return sticker_status_; }
    bool answerPoll(int64_t message, const std::vector<int>& options);
    std::string pollStatus(int64_t message) const;
    bool pollPending(int64_t message) const;
    void loadReactions(int64_t message_id);
    void viewMessage(int64_t message_id);
    void messagesPresented();
    void toggleReaction(int64_t message_id, const std::string& emoji);
    void ensureReactionMedia(size_t index);
    std::string reactionIcon(size_t index) const;
    void ensureMessageReactionMedia();
    std::string customReactionIcon(int64_t custom_emoji_id) const;
    const std::vector<std::string>& reactionChoices() const { return reaction_choices_; }
    const std::string& reactionStatus() const { return reaction_status_; }
    void leaveChat();
    void ensureChatMedia(size_t index);
    void ensureMessageMedia(size_t index);
    void setAnimationTarget(int index); // -1 cancels the opened animation download
    void setVoiceTarget(int index);
    void ensurePhotoThumbnail(size_t message_index, size_t photo_index);
    void ensureFullPhoto(size_t message_index, size_t photo_index);

private:
    void syncOpenedChat();
    int64_t opened_chat_id_ = 0;
    bool logging_out_ = false;
    pthread_t cleanup_thread_{};
    bool cleanup_started_ = false;
    std::atomic<int> cleanup_result_{0}; // 0 pending, 1 success, -1 failure.
    static void* cleanupEntry(void* self);
    void clearAccountMemory();
    struct PollRequest { bool pending=false; std::string status; };
    std::map<std::pair<int64_t,int64_t>, PollRequest> poll_requests_;
    std::string video_note_status_;

    bool proxy_enabled_ = false;
    ProxyConfig proxy_;
    std::string proxy_status_;
    int proxy_generation_ = 0;
    int proxy_check_generation_ = 0;
    int64_t proxy_check_deadline_ = 0;
    std::string proxy_check_status_;
    void applyProxy();
    // The database on disk is protected by one of: nothing, the console-derived key (legacy marker
    // v1) or the vault key (marker v2). It is re-keyed when the vault is switched on or off.
    std::string currentDatabaseKey() const;
    std::string targetDatabaseKey() const;
    int databaseMarker() const;       // 0 none, 1 console key, 2 vault key
    int targetDatabaseMarker() const;
    bool database_key_migrating_ = false;
    void migrateDatabaseKey();
    std::string auth_detail_, qr_link_;
    bool auth_can_resend_ = false;
    std::string auth_next_via_;
    int64_t auth_resend_at_ = 0;
    vault_sweep::Sealer sealer_;
    std::vector<ForumTopicItem> topics_;
    std::string topics_status_, forum_title_;
    bool topics_open_ = false, topics_pending_ = false, topics_end_ = false;
    int32_t current_topic_ = 0, topics_date_ = 0, topics_offset_ = 0;
    int64_t topics_message_ = 0;
    unsigned topics_generation_ = 0;
    bool matchesTopic(void* message) const;
    std::vector<SendStickerItem> stickers_;
    std::vector<ChatFolderItem> sticker_packs_;
    std::vector<int64_t> sticker_set_ids_;
    bool sticker_packs_fresh_ = false;
    std::unordered_map<int64_t, int64_t> sticker_icon_retry_;
    // supergroup_id -> is_forum from updateSupergroup/getSupergroup; avoids a
    // TDLib round trip before the first history request of an ordinary group.
    std::unordered_map<int64_t, bool> supergroup_forum_;
    int64_t sticker_items_pending_set_ = 0;
    void applyStickerPackIcon(size_t index, void* set);
    std::string sticker_status_;
    unsigned sticker_generation_ = 0, sticker_session_ = 0;
    std::vector<std::string> reaction_choices_;
    std::unordered_map<std::string,int64_t> reaction_custom_choices_;
    struct ReactionPreview { int32_t file=0; std::string path; };
    std::unordered_map<int64_t,ReactionPreview> reaction_previews_;
    int64_t viewed_message_=0, viewed_message_at_=0;
    int64_t chat_open_at_=0;
    bool chat_present_pending_=false, chat_live_present_pending_=false, chat_live_ready_=false;
    std::string reaction_status_;
    int64_t reaction_message_ = 0, reaction_requested_at_ = 0;
    bool reaction_pending_ = false, reaction_loading_ = false, reaction_loaded_ = false;
    bool cache_maintenance_ = false;
    std::unordered_map<int64_t, std::string> user_names_;
    struct UserPresence { std::string type; int64_t time = 0; bool bot = false; };
    std::unordered_map<int64_t, UserPresence> user_presence_;
    void parseUserStatus(int64_t id, void* status);
    void restoreStickerCache(void* snapshot);
    void applyStickerResponse(void* root, bool packs);
    void applyStickerSlice();
    int sticker_selected_pack_=-1;
    bool sticker_selection_pending_=false, sticker_selection_loaded_=false;
    void* sticker_apply_root_ = nullptr;
    size_t sticker_apply_offset_ = 0;
    int64_t sticker_apply_chat_ = 0;
    unsigned sticker_apply_generation_ = 0, sticker_apply_session_ = 0;
    struct SenderPhoto { int32_t file=0; std::string path; bool fresh=false; };
    std::unordered_map<int64_t, SenderPhoto> user_photos_;
    std::unordered_map<std::string,int64_t> sender_requests_;
    void ensureSenderMedia(const MessageItem& message);
    void parseUser(void* user);
    MediaCache media_cache_;
    UiCache ui_cache_;
    std::unordered_map<int64_t, int64_t> avatar_metadata_retry_;
    bool history_cached_ = false;
    std::vector<ChatFolderItem> folders_;
    int32_t selected_folder_ = 0;
    unsigned list_generation_ = 0;
    unsigned refresh_generation_ = 0;
    int64_t chats_request_deadline_ = 0, refresh_deadline_ = 0;
    bool connection_ready_ = false;
    void resetListRequests(int64_t now);
    void recoverListRequests(int64_t now);
    size_t selected_chat_count_ = 0;
    void sendListRequest(const char* method, const char* kind, int64_t chat_id = 0);
    void parseFolders(void* update);
    int32_t active_animation_file_ = 0;
    int32_t active_voice_file_ = 0;
    unsigned voice_download_generation_ = 0;
    unsigned animation_download_generation_ = 0;
    int64_t animation_started_at_ = 0, animation_progress_ = 0, animation_progress_at_ = 0;
    std::unordered_map<int64_t, int64_t> read_outbox_;
    std::unordered_set<int64_t> replaced_message_ids_;
    void storeMessage(void* message, int64_t old_id = 0);
    void updateReadOutbox(int64_t chat_id, int64_t last_id);
    bool refresh_pending_ = false, refresh_manual_ = false;
    bool refresh_top_ = false;
    std::vector<int64_t> refresh_before_;
    int64_t next_update_check_ = 0, notice_until_ = 0;
    std::string notice_;
    unsigned link_generation_ = 0;
    bool link_chat_opened_ = false;
    std::unordered_set<int64_t> refresh_ids_;
    void checkChatUpdates(bool manual);
    void finishChatRefresh();
    void updateChatPositions(int64_t id, void* positions);
    void noteLastMessage(int64_t id, void* message);
    bool chats_pending_ = false, chats_end_ = false;
    size_t chats_target_ = 15;
    struct FolderWindow { size_t target = 15; bool end = false; };
    std::unordered_map<int32_t, FolderWindow> folder_windows_;
    int64_t next_chats_request_ = 0;
    std::string chats_error_;
    void requestChatsPage();
    pthread_t worker_{};
    pthread_t cache_worker_{};
    std::atomic<bool> cache_worker_started_{false};
    bool restore_on_cache_worker_ = false; // Set before starting that worker.
    bool cache_restore_allowed_ = true, live_folders_received_ = false; // UI only.
    void* restored_snapshot_ = nullptr; // Ownership protected by queue_mutex_.
    bool full_cache_loaded_ = false; // Storage worker only (or synchronous restore).
    void* applying_snapshot_ = nullptr; // UI owns the incremental snapshot.
    unsigned cache_apply_stage_ = 0;
    bool cache_restore_chats_ = false;
    void beginCacheApply(void* snapshot);
    void applyCacheSlice();
    struct CacheDemand { int64_t chat = 0; unsigned history = 0, session = 0, stickers = 0; int index = -1; bool pending = false; };
    CacheDemand cache_demand_; // Latest on-demand offline history/stickers; queue_mutex_.
    CacheDemand cache_result_key_;
    void* cache_result_ = nullptr;
    void requestCachedDetails(int sticker_index = -1);
    void applyCachedDetails();
    void prepareCachedDetails(); // Storage worker.
    std::deque<int64_t> cached_avatar_requests_; // queue_mutex_, visible offline rows only.
    std::deque<std::pair<int64_t, std::string>> cached_avatar_results_;
    std::unordered_set<int64_t> cached_avatar_seen_;
    void prepareCachedAvatar();
    void applyCachedAvatar();
    void* prepareCache(); // Background lightweight index; legacy migration if absent.
    void applyRestoredCache(void* snapshot); // UI; takes ownership, keeps live data.
    static void* cacheEntry(void* self);
    static bool cacheLoadProceed(void* self);
    int64_t cache_service_at_ = 0; // Storage only.
    int64_t cache_load_slice_ = 0; // Storage only.
    pthread_t parser_{};
    pthread_mutex_t queue_mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> worker_started_{false};
    std::atomic<bool> parser_started_{false};
    std::atomic<int> request_batch_size_{16};
    struct Request { std::string text; int priority; int64_t queued; bool first_history=false, backlog_exempt=false; int32_t file_id=0; bool forum_check=false; int64_t selected=0; };
    std::deque<Request> outgoing_;
    struct Incoming { std::string text; int64_t received = 0; bool priority = false; };
    size_t incoming_priority_ = 0; // Leading priority entries of incoming_; protected by queue_mutex_.
    std::deque<Incoming> incoming_;
    size_t incoming_bytes_ = 0, parsed_wire_bytes_ = 0; // Protected by queue_mutex_.
    void sampleQueues(); // queue_mutex_ held; diagnostics never wait.
    std::atomic<int64_t> interaction_until_{0};
    // Set while the open chat waits for its first history page (Worker/Parser).
    std::atomic<int64_t> foreground_until_{0};
    media_trash::Sweeper trash_; // Storage thread only.
    int64_t folder_selected_at_ = 0;
    bool folder_first_seen_ = false;
    unsigned folder_requests_ = 0;
    TdEventQueue parsed_;
    unsigned event_turn_ = 0; // Fairness survives short pumps and continuous input.
    static void* workerEntry(void* self);
    static void* parserEntry(void* self);
    void runParser();
    void runWorker();
    std::vector<MessageItem> history_items_;
    std::unordered_map<int64_t, std::vector<MessageItem::Reaction>> cached_reactions_;
    unsigned int history_generation_ = 0;
    int64_t message_jump_ = 0;
    unsigned message_jump_generation_ = 0;
    bool history_pending_ = false, history_done_ = true, history_end_ = false;
    int history_target_ = 60;
    int64_t history_cursor_ = 0;
    std::string history_error_;
    void requestHistoryPage();
    void showTopicsList(); // Forum: invalidate history, show the topic list.
    void rebuildHistory();
    void* client_;
    bool ready_;
    bool closed_;
    bool requested_chats_;
    bool tdlib_parameters_sent_;
    InputKind input_kind_;
    int64_t current_chat_id_;
    std::string current_chat_title_;
    std::string status_;
    std::string error_;
    std::vector<ChatItem> chats_;
    std::vector<ChatItem> known_chats_;
    void publishChats();
    std::map<int32_t, std::vector<ChatItem>> startup_chat_snapshots_;
    void finishStartupChatList();
    std::vector<MessageItem> messages_;
    std::unordered_set<int32_t> pending_files_;
    struct DocumentDownload { std::string directory, name; };
    std::unordered_map<int32_t, DocumentDownload> document_downloads_;
    std::string document_status_;
    std::unordered_set<int32_t> list_avatar_files_; // Chat-list avatars requested from TDLib.
    std::unordered_map<int32_t, int> file_priorities_;
    std::unordered_map<int32_t, int64_t> file_update_times_;
    MediaUploads uploads_;
    std::atomic<bool> uploads_pending_{false}; // Cache I/O yields to media sends.
    int64_t upload_log_time_ = 0;
    std::string upload_log_snapshot_;
    std::deque<std::string> upload_logs_;
    void logUploads(bool force = false);
    void flushUploadLogs();

    void sendJson(void* json_root);
    void sendRaw(const char* json, int priority = 3, bool first_history=false, bool backlog_exempt=false, int32_t file_id=0, bool forum_check=false);
    void process(void* json_root);
    void processAuth(void* root);
    void parseChat(void* chat_obj);
    MessageItem parseMessage(void* msg_obj);
    void requestFile(int32_t file_id, int priority = 16, bool visible_avatar=false);
    void dropListAvatarDownloads();
    void updateFile(void* file_obj);
    void setInput(InputKind kind, const std::string& status);
};
