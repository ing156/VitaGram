#pragma once
#include "media_cache.h"
#include "ui_cache.h"
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
struct ChatFolderItem { int32_t id; std::string title; };

struct MessageItem {
    struct Reaction { std::string emoji; int count = 0; bool chosen = false; };
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
    int32_t media_file_id = 0;
    std::string media_path;
    bool sticker = false;
    bool animation = false;
    bool video = false, video_note = false; // Shares the animation download/preview fields.
    int video_duration = 0;
    bool voice = false;
    bool audio = false; // Ordinary audio uses the same download/playback fields as voice.
    std::string audio_title, audio_performer;
    int voice_duration = 0;
    int32_t voice_file_id = 0;
    int64_t voice_size = 0, voice_downloaded = 0;
    std::string voice_path, voice_error;
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

    bool start();
    void restoreCache();
    bool canBrowse() const { return ready_ || !known_chats_.empty(); }
    void pump();
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
    const std::string& lastError() const { return error_; }
    std::string mediaCacheStatus() { return media_cache_.status(); }

    InputKind inputKind() const { return input_kind_; }
    std::string inputPrompt() const;
    bool inputSecret() const { return input_kind_ == InputKind::Password; }
    void submitInput(const std::string& value);
    void cancelInput();

    const std::vector<ChatItem>& chats() const { return chats_; }
    const std::vector<MessageItem>& messages() const { return messages_; }
    int64_t currentChatId() const { return current_chat_id_; }
    const std::string& currentChatTitle() const { return current_chat_title_; }
    std::string currentUserStatus() const;
    bool currentChatIsGroup() const;
    std::string senderName(const MessageItem& message) const;

    void refreshChats();
    const std::vector<ChatFolderItem>& folders() const { return folders_; }
    int32_t selectedFolder() const { return selected_folder_; }
    void selectFolder(int32_t id);
    std::string updateNotice() const;
    bool takeChatTopRequest() { bool result = refresh_top_; refresh_top_ = false; return result; }
    void loadMoreChats();
    bool chatsLoading() const { return ready_ && !chats_end_ && chats_error_.empty() && chats_.size() < chats_target_; }
    bool chatsEnd() const { return chats_end_ && chats_.size() == selected_chat_count_; }
    const std::string& chatsError() const { return chats_error_; }
    void openChat(int64_t chat_id, const std::string& title);
    void refreshHistory();
    void requestCompose();
    void loadReactions(int64_t message_id);
    void toggleReaction(int64_t message_id, const std::string& emoji);
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
    std::vector<std::string> reaction_choices_;
    std::string reaction_status_;
    int64_t reaction_message_ = 0;
    bool reaction_pending_ = false;
    bool cache_maintenance_ = false;
    std::unordered_map<int64_t, std::string> user_names_;
    struct UserPresence { std::string type; int64_t time = 0; bool bot = false; };
    std::unordered_map<int64_t, UserPresence> user_presence_;
    void parseUserStatus(int64_t id, void* status);
    std::unordered_set<std::string> sender_requests_;
    void parseUser(void* user);
    MediaCache media_cache_;
    UiCache ui_cache_;
    std::unordered_map<int64_t, int64_t> avatar_metadata_retry_;
    bool history_cached_ = false;
    std::vector<ChatFolderItem> folders_;
    int32_t selected_folder_ = 0;
    unsigned list_generation_ = 0;
    size_t selected_chat_count_ = 0;
    void sendListRequest(const char* method, const char* kind, int64_t chat_id = 0);
    void parseFolders(void* update);
    int32_t active_animation_file_ = 0;
    int32_t active_voice_file_ = 0;
    unsigned voice_download_generation_ = 0;
    unsigned animation_download_generation_ = 0;
    std::unordered_map<int64_t, int64_t> read_outbox_;
    std::unordered_set<int64_t> replaced_message_ids_;
    void storeMessage(void* message, int64_t old_id = 0);
    void updateReadOutbox(int64_t chat_id, int64_t last_id);
    bool refresh_pending_ = false, refresh_manual_ = false;
    bool refresh_top_ = false;
    std::vector<int64_t> refresh_before_;
    int64_t next_update_check_ = 0, notice_until_ = 0;
    std::string notice_;
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
    bool cache_worker_started_ = false;
    static void* cacheEntry(void* self);
    pthread_t parser_{};
    pthread_mutex_t queue_mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::atomic<bool> stopping_{false};
    bool worker_started_ = false;
    bool parser_started_ = false;
    struct Request { std::string text; int priority; int64_t queued; };
    std::deque<Request> outgoing_;
    std::deque<std::string> incoming_;
    std::deque<void*> parsed_;
    static void* workerEntry(void* self);
    static void* parserEntry(void* self);
    void runParser();
    void runWorker();
    std::vector<MessageItem> history_items_;
    unsigned int history_generation_ = 0;
    bool history_pending_ = false, history_done_ = true, history_end_ = false;
    int history_target_ = 60;
    int64_t history_cursor_ = 0;
    std::string history_error_;
    void requestHistoryPage();
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
    std::vector<MessageItem> messages_;
    std::unordered_set<int32_t> pending_files_;
    std::unordered_map<int32_t, int> file_priorities_;

    void sendJson(void* json_root);
    void sendRaw(const char* json, int priority = 3);
    void process(void* json_root);
    void processAuth(void* root);
    void parseChat(void* chat_obj);
    MessageItem parseMessage(void* msg_obj);
    void requestFile(int32_t file_id, int priority = 16);
    void updateFile(void* file_obj);
    void setInput(InputKind kind, const std::string& status);
};
