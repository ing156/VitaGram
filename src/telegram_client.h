#pragma once
#include "i18n.h"
#include "proxy_config.h"
#include "vault_sweep.h"
#include "media_uploads.h"
#include "td_event_queue.h"
#include "poll.h"
#include "bot_buttons.h"
#include "peer_colors.h"
#include "gif_catalog.h"
#include <map>
#include <set>
#include "media_cache.h"
#include "ui_cache.h"
#include "media_trash.h"
#include "download_stream.h"
#include <set>
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
    peer_colors::Peer colors;
    int64_t supergroup_id = 0;
    int64_t user_id = 0;
    bool group = false;
    bool channel = false;
    int64_t basic_group_id = 0;
    bool cached_metadata = false;
    bool can_send = true;   // chat-wide default permission can_send_basic_messages
    std::unordered_map<int32_t, int64_t> list_orders; // 0 = main, >0 = folder, kArchiveFolderId = archive
    int64_t id = 0;
    int64_t order = 0;
    int64_t last_message_id = 0;
    int unread_count = 0;
    bool marked_unread = false;
    int64_t last_read_inbox = 0; // Synchronized Telegram read position.
    std::string title;
    int32_t avatar_file_id = 0;
    std::string avatar_path;
    bool avatar_stale = false; // avatar_path is an earlier picture shown until avatar_file_id downloads.
};

struct PhotoItem {
    int width = 0, height = 0;
    int32_t thumbnail_file_id = 0;
    std::string thumbnail_path;
    int32_t full_file_id = 0;
    std::string full_path;
};

enum class DeliveryState { Pending, Sent, Read, Failed };
struct ForumTopicItem { int32_t id = 0; std::string title; int unread = 0; bool closed = false; int64_t last_read_inbox = 0; };
struct SendStickerItem { int32_t file = 0, preview = 0; int width = 0, height = 0; bool animated = false; std::string emoji, path, full_path; int64_t set_id = 0; };
// Folder tab id of the Archive (TDLib chatListArchive); TDLib folder ids are positive.
constexpr int32_t kArchiveFolderId = -2;
struct ChatFolderItem {
    int32_t id = 0;
    i18n::Text title;
    int32_t icon_file_id = 0;
    std::string icon_path;
};

// A link inside MessageItem::text: UTF-8 byte range [begin, end) taken from TDLib text entities.
struct LinkSpan { int begin = 0, end = 0; std::string url; };

enum TextStyle { TextBold=1, TextItalic=2, TextUnderline=4, TextStrike=8, TextSpoiler=16, TextCode=32, TextQuote=64 };
struct StyleSpan { int begin, end; unsigned style; };

struct VideoVariant {
    int width = 0, height = 0;
    int32_t file = 0;
    int64_t size = 0, downloaded = 0;
    bool alternative = false;
    std::string path;
    download_stream::Shared stream;
};

struct MessageItem {
    int64_t via_bot_user_id = 0;
    bot_buttons::Markup buttons;
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
    std::vector<StyleSpan> styles;
    i18n::Text text;
    i18n::Text media_label;
    bool pin_message = false;
    bool service_message = false;
    bool basic_group_created = false;
    bool member_removed = false;
    int64_t removed_member_id = 0;
    int64_t pinned_message_id = 0;
    int32_t media_file_id = 0;
    std::string media_path;
    std::string media_fallback; // Embedded GIF JPEG: immediate preview without a download.
    bool media_video_preview = false; // Separate MP4/GIF thumbnail, decoded by the image worker.
    bool document = false;
    std::string document_name;
    int64_t document_size = 0, document_downloaded = 0;
    bool sticker = false;
    int64_t sticker_set_id = 0;
    std::vector<int64_t> custom_emoji_ids;
    bool animation = false;
    bool video = false, video_note = false; // Shares the animation download/preview fields.
    int video_duration = 0;
    int video_width = 0, video_height = 0;
    bool video_alternative = false;
    std::vector<VideoVariant> video_variants;
    bool voice = false;
    bool audio = false; // Ordinary audio uses the same download/playback fields as voice.
    i18n::Text audio_title; std::string audio_performer;
    int voice_duration = 0;
    int32_t voice_file_id = 0;
    int64_t voice_size = 0, voice_downloaded = 0;
    std::string voice_path; i18n::Text voice_error;
    download_stream::Shared voice_stream;
    std::string transcription, transcription_error;
    std::vector<uint8_t> waveform;   // voice notes: 0..31 amplitudes decoded from voiceNote.waveform
    bool transcription_pending = false;
    bool transcription_complete = false;
    int32_t animation_file_id = 0;
    int64_t animation_size = 0, animation_downloaded = 0;
    std::string animation_path; i18n::Text animation_error;
    download_stream::Shared animation_stream;
    std::vector<PhotoItem> photos;
    bool spoiler = false;   // TDLib has_spoiler: previews stay blurred until the user confirms (session only)
    std::vector<std::string> links;
    // Reply reference (TDLib reply_to). reply_sender/reply_text are filled from history, a cached lookup or the
    // reply content itself; empty text with reply_missing == false means "still loading".
    // Forward origin (TDLib forward_info.origin): a user, a chat/channel, or a hidden sender with only a name.
    bool forwarded = false, forward_from_chat = false;
    int64_t forward_origin_id = 0;
    std::string forward_hidden_name, forward_source;   // forward_source is resolved at display time
    int64_t reply_to_id = 0, reply_chat_id = 0;
    int64_t reply_sender_id = 0;
    bool reply_sender_chat = false;
    std::string reply_quote, reply_origin_name, reply_sender; i18n::Text reply_text;
    bool reply_missing = false;
    // Discussion thread of a channel post (TDLib interaction_info.reply_info): present only when the post has a
    // linked discussion. comments_message_id is the message that carries it (the first one of a merged album).
    bool channel_post = false, has_comments = false;
    int comment_count = 0;
    int64_t comments_message_id = 0;
    std::vector<LinkSpan> link_spans;   // valid only when they index `text` (plain text/caption messages)
    std::string preview_url, preview_site, preview_title, preview_description;
    void refreshLanguage() {
        text.refresh(); media_label.refresh(); audio_title.refresh();
        voice_error.refresh(); animation_error.refresh(); reply_text.refresh();
    }

};

enum class InputKind {
    None,
    Phone,
    Code,
    Password,
    FirstName,
    Email,
    EmailCode,
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
    const i18n::Text& proxyStatus() const { return proxy_status_; }
    void checkProxy(const ProxyConfig& proxy);
    void checkProxy(const std::string& server, int port, const std::string& secret) {
        ProxyConfig proxy; proxy.server = server; proxy.port = port; proxy.secret = secret;
        checkProxy(proxy);
    }
    void cancelProxyCheck();
    // True when TDLib's database is protected with the key it should have now (vault key while the
    // vault is on, console key otherwise). False while a re-key is pending or running.
    bool databaseKeyCurrent() const;
    const i18n::Text& proxyCheckStatus() const { return proxy_check_status_; }
    void setRequestBatchSize(int size) { request_batch_size_.store(std::max(1, std::min(64, size))); }
    void restoreCache(); // UI, before pump() and before the cache writer starts.
    bool restoreCacheAsync(); // Startup: keep UI/TDLib running during cache reads.
    bool canBrowse() const { return ready_ || !known_chats_.empty(); }
    void pump(bool interactive = false);
    void prioritizeInteraction(int64_t now) { interaction_until_.store(now + 150000); }
    void close();
    void logout();
    void optimizeCache(int days, int megabytes, bool manual = false);
    const std::string& cacheCleanupError() const { return cache_cleanup_error_; }
    bool cacheMaintenance() const { return cache_maintenance_; }
    std::string currentChatAvatar() const;
    void stopWorker();
    void loadOlder(bool open_post = false);
    void loadNewer(bool open_post = false);
    bool historyWindow() const { return history_window_; }
    bool selectVideoQuality(int index, int variant_index);
    void setVideoQuality(int value) { video_quality_ = std::clamp(value, 0, 3); }
    void setChatOpenPosition(int value) { chat_open_position_ = std::clamp(value, 0, 2); }
    bool historyAnchorFar() const { return history_anchor_far_; }
    void acceptHistoryAnchorFar();
    void retryHistoryOpening();
    int historyPercent() const { return history_done_ ? 100 : std::min(99, (int)history_items_.size() * 100 / history_target_); }
    bool historyLoading() const { return current_chat_id_ && !history_done_; }
    bool historyAnchorPending() const { return history_anchor_ != 0; }
    const i18n::Text& historyError() const { return history_error_; }

    bool ready() const { return ready_; }
    bool liveListShown() const { return live_list_shown_; } // The first loadChats reply arrived.
    bool liveRowsShown() const { return live_rows_shown_; } // The visible rows switched from the startup index to live data.
    bool closed() const { return closed_; }
    const i18n::Text& status() const { return status_; }
    std::string uploadStatus() const;
    void dismissStatusNotices() {
        uploads_.dismissCompleted();
        bot_status_.clear();
        notice_.clear(); notice_until_ = 0;
    }
    const i18n::Text& lastError() const { return error_; }

    InputKind inputKind() const { return input_kind_; }
    std::string inputPrompt() const;
    bool inputSecret() const { return input_kind_ == InputKind::Password; }
    void submitInput(const std::string& value);
    const bot_buttons::Markup& botMenu() const;
    const MessageItem& botKeyboard() const { return bot_keyboard_; }
    bool activateBotButton(int64_t message, int row, int column, bool keyboard);
    bool followingBotReplies() const { return bot_reply_focus_generation_ == history_generation_; }
    bool takeBotReplyFocus() { bool focus = bot_reply_focus_pending_ && followingBotReplies(); bot_reply_focus_pending_ = false; return focus; }
    std::string takeBotLink() { auto result = std::move(bot_link_); bot_link_.clear(); return result; }
    const i18n::Text& botStatus() const { return bot_status_; }
    std::string takeBotAlert() { auto result = std::move(bot_alert_); bot_alert_.clear(); return result; }

    struct BotWebApp {
        enum Phase { Closed, Confirm, Loading, Ready, Failed } phase = Closed;
        enum Source { Button, MainLink, NamedLink } source = Button;
        int64_t bot = 0, chat = 0, topic = 0, thread = 0, launch = 0;
        bool keyboard = false, same_origin = true;
        std::string label, url, signed_url, error;
        std::string short_name, start_parameter, mode = "webAppOpenModeFullSize";
    };
    const BotWebApp& botWebApp() const { return bot_webapp_; }
    void openBotWebApp(void* theme);
    void closeBotWebApp();
    bool sendBotWebAppData(const std::string& data);

    // Sign-in helpers: where the code was sent, step back to the phone number, ask for a new code.
    const i18n::Text& authDetail() const { return auth_detail_; }
    bool canResendCode() const;
    // "Next: SMS in 42 s" / "Press triangle to get it by SMS"; empty when no other method exists.
    std::string authResendInfo() const;
    bool canGoBackInput() const { return input_kind_ == InputKind::Code || input_kind_ == InputKind::Password || input_kind_ == InputKind::FirstName ||
        input_kind_ == InputKind::Email || input_kind_ == InputKind::EmailCode; }
    // QR sign-in: Telegram's tg://login link while waiting for the phone to confirm (empty otherwise).
    const std::string& qrLink() const { return qr_link_; }
    bool canRequestQr() const { return !ready_ && (input_kind_ == InputKind::Phone || input_kind_ == InputKind::Code || input_kind_ == InputKind::Password ||
        input_kind_ == InputKind::Email || input_kind_ == InputKind::EmailCode); }
    void requestQr();
    void backInput();
    void resendCode();
    void cancelInput();

    const std::vector<ChatItem>& chats() const { return chats_; }
    const std::vector<MessageItem>& messages() const { return messages_; }
    void downloadDocument(const MessageItem& message, const std::string& directory);
    bool hasSavedDocuments() const { return !saved_documents_.empty(); }
    std::string takeSavedDocument() {
        if (saved_documents_.empty()) return {};
        std::string path = std::move(saved_documents_.front());
        saved_documents_.pop_front();
        return path;
    }
    const i18n::Text& documentStatus() const { return document_status_; }
    int64_t currentChatId() const { return current_chat_id_; }
    i18n::Text currentChatTitle() const;
    bool currentChatIsDeleted() const;
    bool chatIsDeleted(const ChatItem& chat) const;
    i18n::Text chatTitle(const ChatItem& chat) const;
    std::string currentUserStatus() const;
    bool currentChatIsGroup() const;
    std::string senderAvatar(const MessageItem& message) const;
    i18n::Text senderName(const MessageItem& message) const;
    bool senderIsDeleted(const MessageItem& message) const;
    peer_colors::Palette senderColors(int64_t id, bool chat, bool dark) const;
    peer_colors::Palette chatColors(const ChatItem& chat, bool dark) const;
    i18n::Text serviceMessageText(const MessageItem& message) const;

    void refreshChats();
    const std::vector<ChatFolderItem>& folders() const { return folders_; }
    int32_t selectedFolder() const { return selected_folder_; }
    void selectFolder(int32_t id);
    void setShowArchive(bool show);
    std::string updateNotice() const;
    // Progress of the latest speech-to-text request (requested, processing, done, error); empty once stale.
    std::string transcriptionStatus() const;
    bool takeChatTopRequest() { bool result = refresh_top_; refresh_top_ = false; return result; }
    void loadMoreChats();
    // The rows come from the previous launch and are being confirmed by Telegram.
    bool chatsFromStartupIndex() const { return startup_chat_snapshots_.count(selected_folder_) > 0; }
    bool chatsLoading() const { return ready_ && !chats_end_ && chats_error_.empty() && (chats_.size() < chats_target_ || startup_chat_snapshots_.count(selected_folder_)); }
    bool chatsEnd() const { return chats_end_ && chats_.size() == selected_chat_count_; }
    int64_t chatsLastUpdated() const {
        auto found = chats_last_updated_.find(selected_folder_);
        return found == chats_last_updated_.end() ? 0 : found->second;
    }
    const i18n::Text& chatsError() const { return chats_error_; }
    void openChat(int64_t chat_id, const i18n::Text& title);
    // UI thread, immediately after committing a new catalog. No network requests.
    void refreshLanguage();
    bool openPinnedMessage(int64_t message_id);
    void openChatDate(int32_t date);
    int64_t takeMessageJump() { int64_t id = message_jump_generation_ == history_generation_ ? message_jump_ : 0; message_jump_ = 0; return id; }
    // Whether the jump taken last should also open the expanded post (true except when returning from comments to a list view).
    bool messageJumpOpensPost() const { return message_jump_post_; }
    // Comments of a channel post are the linked discussion's thread (getMessageThread + getMessageThreadHistory).
    bool openComments(int64_t message_id, bool from_post);
    bool inComments() const { return current_thread_ != 0; }
    int64_t currentThreadId() const { return current_thread_; }
    void closeComments();   // back to the channel, positioned on the post the comments belong to
    bool openTelegramLink(const std::string& url);
    bool takeLinkChatOpened() { bool opened = link_chat_opened_; link_chat_opened_ = false; return opened; }
    bool topicsOpen() const { return topics_open_; }
    const std::vector<ForumTopicItem>& topics() const { return topics_; }
    const i18n::Text& topicsStatus() const { return topics_status_; }
    void loadTopics(bool refresh = false);
    void openTopic(size_t index);
    void backToTopics();
    bool inTopic() const { return current_topic_ != 0; }
    void refreshHistory();
    void requestCompose();
    bool requestTranscription(int64_t message_id);
    bool sendVideoNoteFile(const std::string& path, int duration, int64_t chat, int32_t topic);
    const i18n::Text& videoNoteStatus() const { return video_note_status_; }
    int32_t currentTopicId() const { return current_topic_; }
    bool sendAudioFile(const std::string& path, int duration, int64_t chat, int32_t topic);
    // kind: 0 photo, 1 video, 2 file. original=true sends a document without recompression.
    bool sendLocalFiles(const std::vector<std::pair<std::string, int>>& files, int64_t chat, int32_t topic, const std::string& caption = "");
    bool sendLocalFile(const std::string& path, int kind, bool original, int64_t chat, int32_t topic, const std::string& caption = "");
    void openStickers();
    void openStickerSet(int64_t set_id, int64_t custom_emoji_id = 0);
    void installStickerSet();
    bool stickerSetOpen() const { return sticker_view_open_; }
    bool stickerSetEmoji() const { return sticker_view_open_ && sticker_view_emoji_; }
    bool stickerSetCanInstall() const { return sticker_view_id_ && !sticker_view_installed_ && !sticker_view_pending_; }
    const std::string& stickerSetTitle() const { return sticker_view_title_; }
    void loadStickerPacks();
    void selectStickerPack(int index);
    void ensureStickerPackIcon(size_t index);
    void ensureSticker(size_t index, bool full = false);
    bool sendSticker(size_t index);
    const std::vector<SendStickerItem>& stickers() const { return stickers_; }
    const std::vector<ChatFolderItem>& stickerPacks() const { return sticker_packs_; }
    const i18n::Text& stickerStatus() const { return sticker_status_; }
    void searchGifs(const std::string& query = "");
    void loadMoreGifs();
    void closeGifs();
    void ensureGif(size_t index, bool full = false);
    bool sendGif(size_t index);
    const GifCatalog& gifs() const { return gifs_; }
    const i18n::Text& gifStatus() const { return gif_status_; }
    bool gifsCurrent() const { return gifs_.chat == current_chat_id_ && gifs_.topic == current_topic_ && gifs_.thread == current_thread_; }
    bool answerPoll(int64_t message, const std::vector<int>& options);
    std::string pollStatus(int64_t message) const;
    bool pollPending(int64_t message) const;
    void loadReactions(int64_t message_id, bool refresh = false);
    void viewMessage(int64_t message_id);
    void messagesPresented(int64_t selected_message = 0);
    void toggleReaction(int64_t message_id, const std::string& emoji);
    void ensureReactionMedia(size_t index);
    std::string reactionIcon(size_t index) const;
    MessageItem::Reaction reactionState(size_t index) const;
    void ensureMessageReactionMedia();
    std::string customReactionIcon(int64_t custom_emoji_id) const;
    const std::vector<std::string>& reactionChoices() const { return reaction_choices_; }
    int reactionTopCount() const { return reaction_top_count_; }
    const i18n::Text& reactionStatus() const { return reaction_status_; }
    void leaveChat();
    // Chat search: local chats, then the server, then public usernames. Results are ChatItem copies from the known chats.
    void searchChats(const std::string& query);
    void clearSearch();
    std::vector<ChatItem> searchResults() const;
    bool searchPending() const { return search_pending_ > 0; }
    const std::string& searchQuery() const { return search_query_; }
    void ensureSearchMedia(int64_t chat_id);
    // Membership of the opened chat (groups and channels only).
    enum class Membership { None, Unknown, Member, Creator, NotMember };
    Membership currentMembership() const;
    void joinCurrentChat();
    // True when the user may post text in the opened chat (private chats, members of groups, admins of channels).
    bool canPostCurrent() const;
    // Reply context is shared by text and attachment workflows.
    void replyTo(int64_t message_id);
    bool setCompositionReply(int64_t message_id);
    int64_t replyTarget() const { return reply_target_; }
    // Forward: remember a message, then copy it into any chat where posting is possible.
    // refresh=false reads cached rights without issuing requests or expiring the visible action.
    bool canDeleteMessage(int64_t message_id, bool refresh = true);
    bool deletePermissionPending(int64_t message_id) const;
    void deleteMessage(int64_t message_id);
    void beginForward(int64_t message_id);
    bool forwardPending() const { return forward_message_ != 0; }
    bool forwardIsHere() const { return forward_message_ && forward_chat_ == current_chat_id_; }
    void cancelForward() { forward_message_ = 0; forward_chat_ = 0; }
    bool pasteForward();
    void leaveCurrentGroup();
    bool currentChatIsPrivate() const;
    // Private chats: remove=true deletes the conversation from the list, false only clears its messages (both for this account).
    void deleteCurrentHistory(bool remove);
    void ensureChatMedia(size_t index);
    void ensureMessageMedia(size_t index);
    void setAnimationTarget(int index, bool spoiler_revealed = false); // -1 cancels the opened animation download
    void setVoiceTarget(int index);
    // Files still playing after their chat was left: their downloads are not cancelled by a changed target.
    void setBackgroundMedia(int32_t voice_file, int32_t animation_file);
    // Up to 64 loaded music tracks around `index`, kept when leaving their chat.
    struct AudioTrack {
        int32_t file = 0; int64_t message = 0; int duration = 0, size = 0;
        i18n::Text title; std::string path; download_stream::Shared stream;
    };
    void startAudioQueue(int index);
    void clearAudioQueue();
    void serviceAudioQueue();                 // requests the current and the next track
    bool advanceAudioQueue(int direction = 1, bool automatic = true);
    void shuffleAudioQueue();
    bool audioShuffle() const { return audio_shuffle_; }
    int audioRepeat() const { return audio_repeat_; } // 0: off, 1: playlist, 2: track
    void cycleAudioRepeat() { audio_repeat_ = (audio_repeat_ + 1) % 3; }
    const AudioTrack* audioTrack() const { return audio_pos_ < audio_queue_.size() ? &audio_queue_[audio_pos_] : nullptr; }
    int audioTrackIndex(int64_t message) const;   // position in messages(), -1 if absent
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
    struct PollRequest { bool pending=false; i18n::Text status; };
    std::map<std::pair<int64_t,int64_t>, PollRequest> poll_requests_;
    i18n::Text video_note_status_;

    bool proxy_enabled_ = false;
    ProxyConfig proxy_;
    i18n::Text proxy_status_;
    int proxy_generation_ = 0;
    int proxy_check_generation_ = 0;
    int64_t proxy_check_deadline_ = 0;
    i18n::Text proxy_check_status_;
    void applyProxy();
    // The database on disk is protected by one of: nothing, the console-derived key (legacy marker
    // v1) or the vault key (marker v2). It is re-keyed when the vault is switched on or off.
    std::string currentDatabaseKey() const;
    std::string targetDatabaseKey() const;
    int databaseMarker() const;       // 0 none, 1 console key, 2 vault key
    int targetDatabaseMarker() const;
    bool database_key_migrating_ = false;
    void migrateDatabaseKey();
    i18n::Text auth_detail_; std::string qr_link_;
    bool auth_can_resend_ = false;
    i18n::Text auth_next_via_;
    int64_t auth_resend_at_ = 0;
    vault_sweep::Sealer sealer_;
    std::vector<ForumTopicItem> topics_;
    i18n::Text topics_status_; std::string forum_title_;
    bool topics_open_ = false, topics_pending_ = false, topics_end_ = false;
    int32_t current_topic_ = 0, topics_date_ = 0, topics_offset_ = 0;
    int64_t topics_message_ = 0;
    unsigned topics_generation_ = 0;
    bool matchesTopic(void* message) const;
    std::vector<SendStickerItem> stickers_;
    bool sticker_view_open_ = false, sticker_view_installed_ = false, sticker_view_pending_ = false, sticker_view_emoji_ = false;
    int64_t sticker_view_id_ = 0;
    std::string sticker_view_title_;
    GifCatalog gifs_;
    i18n::Text gif_status_;
    std::string gif_request_kind_;
    std::unordered_map<int64_t, bot_buttons::Markup> bot_menus_;
    std::unordered_set<int64_t> bot_info_requested_;
    std::unordered_map<int64_t, int64_t> bot_keyboard_ids_;
    MessageItem bot_keyboard_;
    i18n::Text bot_status_;
    std::string bot_link_, bot_alert_;
    int64_t bot_action_generation_ = 0, bot_keyboard_generation_ = 0;
    int64_t bot_reply_focus_generation_ = -1;
    bool bot_reply_focus_pending_ = false;
    void focusBotReplies();
    BotWebApp bot_webapp_;
    int64_t bot_webapp_generation_ = 0;
    std::unordered_set<int64_t> restricted_bots_;
    void requestBotKeyboard();
    int64_t gif_bot_ = 0;
    void requestGifResults(const std::string& offset);
    std::vector<ChatFolderItem> sticker_packs_;
    std::vector<int64_t> sticker_set_ids_;
    bool sticker_packs_fresh_ = false;
    std::unordered_map<int64_t, int64_t> sticker_icon_retry_;
    // supergroup_id -> is_forum from updateSupergroup/getSupergroup; avoids a
    // TDLib round trip before the first history request of an ordinary group.
    std::unordered_map<int64_t, bool> supergroup_forum_;
    int64_t sticker_items_pending_set_ = 0;
    void applyStickerPackIcon(size_t index, void* set);
    i18n::Text sticker_status_;
    unsigned sticker_generation_ = 0, sticker_session_ = 0;
    std::vector<std::string> reaction_choices_;
    int reaction_top_count_ = 0;
    std::unordered_map<std::string,int64_t> reaction_custom_choices_;
    struct ReactionPreview { int32_t file=0; std::string path; };
    std::unordered_map<int64_t,ReactionPreview> reaction_previews_;
    int64_t viewed_message_=0, viewed_message_at_=0;
    int64_t chat_open_at_=0;
    bool chat_present_pending_=false, chat_live_present_pending_=false, chat_live_ready_=false;
    i18n::Text reaction_status_;
    int64_t reaction_message_ = 0, reaction_requested_at_ = 0;
    bool reaction_pending_ = false, reaction_loading_ = false, reaction_loaded_ = false;
    bool cache_maintenance_ = false;
    std::string cache_cleanup_error_;
    std::unordered_map<int64_t, std::string> user_names_;
    std::unordered_map<int64_t, peer_colors::Peer> user_colors_;
    peer_colors::Colors accent_colors_;
    bool live_accent_colors_ = false;
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
    struct SenderPhoto { int32_t file=0; std::string path; bool fresh=false; bool deleted=false; };
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
    std::unordered_map<int32_t, int64_t> chats_last_updated_;
    bool show_archive_ = true;
    unsigned list_generation_ = 0;
    unsigned refresh_generation_ = 0;
    int64_t chats_request_deadline_ = 0, refresh_deadline_ = 0;
    bool connection_ready_ = false;
    void resetListRequests(int64_t now);
    void recoverListRequests(int64_t now);
    void resumeListRequests(int64_t now);
    size_t selected_chat_count_ = 0;
    void sendListRequest(const char* method, const char* kind, int64_t chat_id = 0);
    void parseFolders(void* update);
    void syncArchiveFolder();
    int32_t active_animation_file_ = 0;
    std::vector<AudioTrack> audio_queue_;
    size_t audio_pos_ = 0;
    bool audio_shuffle_ = false;
    int audio_repeat_ = 0;
    int64_t audio_chat_ = 0;
    bool inAudioQueue(int32_t file) const;
    int32_t active_voice_file_ = 0, background_voice_file_ = 0, background_animation_file_ = 0;
    unsigned voice_download_generation_ = 0;
    unsigned animation_download_generation_ = 0;
    int64_t animation_started_at_ = 0, animation_progress_ = 0, animation_progress_at_ = 0;
    int64_t animation_rate_at_ = 0, animation_rate_downloaded_ = 0, animation_rate_prefix_ = 0;
    std::unordered_map<int64_t, int64_t> read_outbox_;
    std::unordered_set<int64_t> replaced_message_ids_;
    void storeMessage(void* message, int64_t old_id = 0);
    void updateReadOutbox(int64_t chat_id, int64_t last_id);
    bool refresh_pending_ = false, refresh_manual_ = false;
    bool refresh_top_ = false;
    std::vector<int64_t> refresh_before_;
    int64_t next_update_check_ = 0, notice_until_ = 0;
    i18n::Text notice_;
    i18n::Text transcription_status_;
    int64_t transcription_status_until_ = 0;
    void setTranscriptionStatus(const i18n::Text& text, int seconds);
    unsigned link_generation_ = 0;
    unsigned search_generation_ = 0;
    int search_pending_ = 0;
    std::string search_query_;
    std::vector<int64_t> search_ids_[3];
    // supergroup / basic group id -> membership learnt from updateSupergroup, updateBasicGroup or getX.
    std::unordered_map<int64_t, Membership> supergroup_member_, basic_group_member_;
    // group id -> may post as a member / as an administrator (channels need the latter)
    struct PostRights { bool member = false, admin = false; };
    std::unordered_map<int64_t, PostRights> supergroup_rights_, basic_group_rights_;
    static PostRights postRightsOf(void* status);
    struct DeletePermission { bool allowed = false, revoke = false, pending = false; uint64_t checked = 0; };
    int64_t account_user_id_ = 0;
    std::map<int64_t, DeletePermission> delete_permissions_;
    bool message_delete_pending_ = false;
    uint64_t message_delete_deadline_ = 0;
    int64_t reply_target_ = 0, forward_chat_ = 0, forward_message_ = 0;
    static Membership membershipOf(void* status);
    bool membership_pending_ = false;
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
    bool live_list_shown_ = false;
    bool live_rows_shown_ = false;
    int64_t startup_pump_until_ = 0; // Larger UI budget for the startup flood, until the first live chat list.
    i18n::Text chats_error_;
    void requestChatsPage();
    pthread_t worker_{};
    pthread_t cache_worker_{};
    std::atomic<bool> cache_worker_started_{false};
    // Set by the Parser when the first loadChats reply arrives; the full cache load waits for it.
    std::atomic<bool> live_list_received_{false};
    bool restore_on_cache_worker_ = false; // Set before starting that worker.
    bool cache_restore_allowed_ = true, live_folders_received_ = false; // UI only.
    void* restored_snapshot_ = nullptr; // Ownership protected by queue_mutex_.
    bool full_cache_loaded_ = false; // Storage worker only (or synchronous restore).
    std::atomic<int64_t> history_preferred_{0}; // Open chat: its history file is merged first.
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
    std::atomic<int> request_batch_size_{64};
    struct Request { i18n::Text text; int priority; int64_t queued; bool first_history=false, backlog_exempt=false; int32_t file_id=0; bool forum_check=false; int64_t selected=0; };
    std::deque<Request> outgoing_;
    struct Incoming { i18n::Text text; int64_t received = 0; bool priority = false; };
    size_t incoming_priority_ = 0; // Leading priority entries of incoming_; protected by queue_mutex_.
    std::deque<Incoming> incoming_;
    size_t incoming_bytes_ = 0, parsed_wire_bytes_ = 0; // Protected by queue_mutex_.
    uint64_t shed_events_ = 0; int64_t shed_reported_ = 0; // Ephemeral updates dropped over budget; queue_mutex_.
    bool receive_paused_ = false; // Over the hard queue limit: TDLib is not run (td_queue_paused); queue_mutex_.
    void sampleQueues(); // queue_mutex_ held; diagnostics never wait.
    std::atomic<int64_t> interaction_until_{0};
    // Set while the open chat waits for its first history page (Worker/Parser).
    std::atomic<int64_t> foreground_until_{0}, video_hold_until_{0};
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
    bool message_jump_post_ = true, next_jump_post_ = true;
    int64_t current_thread_ = 0, pending_thread_ = 0;
    struct CommentsReturn { int64_t chat = 0; std::string title; int64_t message = 0; bool post = false; };
    CommentsReturn comments_return_, pending_return_;
    std::set<int64_t> comment_probed_;   // posts already probed for comment info in the open chat
    void probeComments();
    std::set<int64_t> channel_full_requested_;   // channels whose full info (linked discussion) TDLib was asked for
    unsigned message_jump_generation_ = 0;
    bool history_pending_ = false, history_done_ = true, history_end_ = false;
    int history_target_ = 60;
    int64_t history_cursor_ = 0;
    int64_t pending_read_anchor_ = 0;
    int video_quality_ = 0; // Maximum allowed quality.
    std::map<std::pair<int64_t, int64_t>, std::pair<int, int>> video_choices_; // Up to 64 explicit choices for this session.
    int chat_open_position_ = 0;
    bool history_anchor_far_ = false, history_window_ = false, history_window_latest_ = false;
    int64_t history_deadline_ = 0;
    void requestHistoryWindow(int64_t anchor, int direction = 0, bool open_post = false);
    int64_t history_anchor_ = 0; // Read position of the chat being opened; 0 when no jump is pending.
    void resolveHistoryAnchor();
    bool historyAnchorSeeking() const;
    i18n::Text history_error_;
    void requestHistoryPage();
    void showTopicsList(); // Forum: invalidate history, show the topic list.
    void rebuildHistory(bool request_missing = true);
    struct ReplyInfo { int64_t sender_id = 0; bool sender_chat = false, missing = false; i18n::Text text; };
    std::map<std::pair<int64_t, int64_t>, ReplyInfo> reply_cache_;   // (chat, message) -> looked-up original
    std::set<std::pair<int64_t, int64_t>> reply_requested_;          // one getMessage per original
    void resolveReplies(bool request_missing = true);
    void* client_;
    bool ready_;
    bool closed_;
    bool requested_chats_;
    bool tdlib_parameters_sent_;
    InputKind input_kind_;
    int64_t current_chat_id_;
    i18n::Text current_chat_title_;
    i18n::Text status_;
    i18n::Text error_;
    std::vector<ChatItem> chats_;
    std::vector<ChatItem> known_chats_;
    void publishChats();
    std::map<int32_t, std::vector<ChatItem>> startup_chat_snapshots_;
    void finishStartupChatList();
    // Until the startup index is installed (or known to be absent), live chats that TDLib
    // announces before loadChats are not shown as a partial list.
    bool startup_list_hold_ = false;
    bool startup_index_read_ = false; // queue_mutex_; set by Storage with restored_snapshot_.
    bool startup_list_dirty_ = false;
    int64_t next_startup_list_save_ = 0;
    void* pending_startup_list_ = nullptr; // queue_mutex_; written by Storage.
    uint64_t pending_startup_generation_ = 0; // queue_mutex_; UiCache generation of the list.
    void queueStartupList(); // UI: the confirmed top of each list for the next launch.
    void writeStartupList(bool final); // Storage.
    ChatItem chatFromJson(void* chat_obj) const;
    std::vector<MessageItem> messages_;
    std::unordered_set<int32_t> pending_files_;
    struct DocumentDownload { std::string directory, name; unsigned generation; bool exporting = false; };
    unsigned document_download_generation_ = 0;
    std::unordered_map<int32_t, DocumentDownload> document_downloads_;
    struct DocumentExport { int32_t id; unsigned generation; std::string source, directory, name; };
    struct DocumentExportResult { int32_t id; unsigned generation; std::string path; bool ok, existed; };
    // Queues cross UI/Storage under queue_mutex_; document_downloads_ stays on UI.
    std::deque<DocumentExport> document_exports_;
    std::deque<DocumentExportResult> document_export_results_;
    int32_t document_export_id_ = 0; // queue_mutex_; current copy progress.
    int document_export_percent_ = 0;
    pthread_t document_export_worker_{};
    bool document_export_started_ = false;
    std::atomic<bool> document_export_stopping_{false};
    void queueDocumentExport(int32_t id, const std::string& source);
    void applyDocumentExports();
    void stopDocumentExports();
    static void* documentExportEntry(void* self);
    i18n::Text document_status_;
    std::deque<std::string> saved_documents_; // Successful explicit exports, consumed by the UI.
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
    std::deque<std::string> video_quality_logs_; // Bounded diagnostic snapshots; only the cache worker writes them.
    void flushVideoQualityLogs();

    void sendJson(void* json_root);
    void sendRaw(const char* json, int priority = 3, bool first_history=false, bool backlog_exempt=false, int32_t file_id=0, bool forum_check=false);
    void process(void* json_root);
    void processAuth(void* root);
    void parseChat(void* chat_obj);
    MessageItem parseMessage(void* msg_obj);
    void requestFile(int32_t file_id, int priority = 16, bool visible_avatar=false);
    void dropListAvatarDownloads();
    void updateFile(void* file_obj, bool stream_update = true);
    void serviceStreamTails();
    download_stream::Shared mediaStream(void* file_obj, bool live_update = false);
    std::unordered_map<int32_t, std::weak_ptr<download_stream::State>> media_streams_;
    void setInput(InputKind kind, const i18n::Text& status);
};
