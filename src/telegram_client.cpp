#include "telegram_client.h"
#include "cpu_scheduler.h"
#include "config.h"

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

static int32_t chat_list_id(json_t* list) {
    const char* type = json_str(list, "@type");
    if (!std::strcmp(type, "chatListMain")) return 0;
    if (!std::strcmp(type, "chatListFolder")) return (int32_t)json_i53(list, "chat_folder_id");
    return -1; // Archive is never the All chats tab.
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

void TelegramClient::restoreCache() {
    media_cache_.load(); ui_cache_.load();
    json_t* users = ui_cache_.users();
    const char* user_key; json_t* user;
    if (json_is_object(users)) json_object_foreach(users, user_key, user) parseUser(user);
    if (users) json_decref(users);
    json_t* cached = ui_cache_.chats();
    media_cache_.resolve(cached); clear_cached_file_ids(cached);
    const char* key; json_t* chat;
    if (json_is_object(cached)) json_object_foreach(cached, key, chat) parseChat(chat);
    if (cached) json_decref(cached);
    for (auto& chat : known_chats_) chat.cached_metadata = true;
    json_t* folders = ui_cache_.folders();
    if (folders) { parseFolders(folders); json_decref(folders); }
    publishChats();
    if (!chats_.empty()) status_ = "Cached chats - connecting...";
}

TelegramClient::TelegramClient()
    : client_(nullptr), ready_(false), closed_(false), requested_chats_(false),
      tdlib_parameters_sent_(false), input_kind_(InputKind::None), current_chat_id_(0) {}

TelegramClient::~TelegramClient() {
    stopWorker();
    pthread_mutex_destroy(&queue_mutex_);
}

void TelegramClient::stopWorker() {
    stopping_ = true;
    if (worker_started_) { pthread_join(worker_, nullptr); worker_started_ = false; }
    if (parser_started_) { pthread_join(parser_, nullptr); parser_started_ = false; }
    if (cache_worker_started_) { pthread_join(cache_worker_, nullptr); cache_worker_started_ = false; }
    for (void* root : parsed_) json_decref(static_cast<json_t*>(root));
    parsed_.clear(); incoming_.clear(); outgoing_.clear();
}

void* TelegramClient::workerEntry(void* self) {
    static_cast<TelegramClient*>(self)->runWorker();
    return nullptr;
}

void* TelegramClient::parserEntry(void* self) {
    static_cast<TelegramClient*>(self)->runParser();
    return nullptr;
}

void TelegramClient::runParser() {
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Parser);
    while (!stopping_) {
        std::string text;
        pthread_mutex_lock(&queue_mutex_);
        if (parsed_.size() < 128 && !incoming_.empty()) {
            text = std::move(incoming_.front()); incoming_.pop_front();
        }
        pthread_mutex_unlock(&queue_mutex_);
        if (text.empty()) { sceKernelDelayThread(2000); continue; }
        json_error_t error;
        json_t* root = json_loads(text.c_str(), 0, &error);
        if (!root) {
            root = json_pack("{s:s,s:i,s:s}", "@type", "error", "code", 500,
                             "message", "Unable to parse TDLib response");
        }
        if (root) {
            // Transfer ownership: only the UI may access/decref this object next.
            pthread_mutex_lock(&queue_mutex_);
            parsed_.push_back(root);
            pthread_mutex_unlock(&queue_mutex_);
        }
    }
}

void* TelegramClient::cacheEntry(void* opaque) {
    auto* self = static_cast<TelegramClient*>(opaque);
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Parser);
    int64_t next = 0;
    while (!self->stopping_) {
        int64_t now = sceKernelGetSystemTimeWide();
        if (now >= next) {
            self->media_cache_.flush(); self->ui_cache_.flush();
            next = now + 5000000;
        }
        sceKernelDelayThread(100000);
    }
    self->media_cache_.flush(); self->ui_cache_.flush();
    return nullptr;
}

void TelegramClient::runWorker() {
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Network);
    // This port's TDLib scheduler is single-threaded: keep every TD call here.
    client_ = td_json_client_create();
    if (!client_) {
        pthread_mutex_lock(&queue_mutex_);
        incoming_.push_back("{\"@type\":\"error\",\"code\":500,\"message\":\"TDLib creation failed\"}");
        pthread_mutex_unlock(&queue_mutex_);
        return;
    }
    while (!stopping_) {
        std::deque<std::string> requests;
        pthread_mutex_lock(&queue_mutex_);
        // Bound each send batch; receiving updates and yielding must not be
        // postponed behind a large burst of thumbnail/history requests.
        for (int i = 0; i < 4 && !outgoing_.empty(); ++i) {
            int64_t now = sceKernelGetSystemTimeWide();
            auto best = outgoing_.begin();
            auto score = [&](const Request& request) {
                // Aging prevents background requests from starving indefinitely.
                return request.priority + std::min<int64_t>(4, (now-request.queued)/2000000);
            };
            for (auto it = outgoing_.begin(); it != outgoing_.end(); ++it)
                if (score(*it) > score(*best)) best = it;
            requests.push_back(std::move(best->text)); outgoing_.erase(best);
        }
        bool full = incoming_.size() >= 128;
        pthread_mutex_unlock(&queue_mutex_);
        for (const auto& request : requests) td_json_client_send(client_, request.c_str());
        bool received = false;
        if (!full) {
            const char* response = td_json_client_receive(client_, 0.0);
            if (response) {
                pthread_mutex_lock(&queue_mutex_);
                incoming_.emplace_back(response);
                pthread_mutex_unlock(&queue_mutex_);
                received = true;
            }
        }
        sceKernelDelayThread(received ? 100 : 2000);
    }
    td_json_client_destroy(client_);
    client_ = nullptr;
}

bool TelegramClient::start() {
    if (VITAGRAM_API_ID <= 0 || std::strstr(VITAGRAM_API_HASH, "PUT_YOUR")) {
        error_ = "Edit src/config.h and set your Telegram api_id/api_hash";
        status_ = "Configuration required";
        return false;
    }

    sceIoMkdir("ux0:data/vitagram", 0777);
    sceIoMkdir(VITAGRAM_DB_DIR, 0777);
    sceIoMkdir(VITAGRAM_FILES_DIR, 0777);

    td_set_log_message_callback(1, nullptr);
    pthread_attr_t attributes;
    stopping_ = false;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 2 * 1024 * 1024);
    int worker_result = pthread_create(&worker_, &attributes, workerEntry, this);
    int parser_result = worker_result == 0 ? pthread_create(&parser_, &attributes, parserEntry, this) : -1;
    int cache_result = parser_result == 0 ? pthread_create(&cache_worker_, &attributes, cacheEntry, this) : -1;
    pthread_attr_destroy(&attributes);
    worker_started_ = worker_result == 0;
    parser_started_ = parser_result == 0;
    cache_worker_started_ = cache_result == 0;
    if (!worker_started_ || !parser_started_ || !cache_worker_started_) {
        error_ = "Telegram worker creation failed";
        stopWorker();
        return false;
    }
    status_ = "TDLib created; waiting for response...";
    sendRaw("{\"@type\":\"getAuthorizationState\",\"@extra\":\"startup_probe\"}");
    return true;
}

void TelegramClient::sendRaw(const char* json, int priority) {
    if (!worker_started_) return;
    pthread_mutex_lock(&queue_mutex_);
    outgoing_.push_back({json, priority, sceKernelGetSystemTimeWide()});
    pthread_mutex_unlock(&queue_mutex_);
}

void TelegramClient::sendJson(void* opaque) {
    json_t* root = (json_t*)opaque;
    const char* method = json_str(root, "@type");
    if (!std::strcmp(method, "sendMessage") && current_topic_)
        json_object_set_new(root, "topic_id", json_pack("{s:s,s:i}", "@type", "messageTopicForum", "forum_topic_id", current_topic_));
    const char* kind = json_str(json_object_get(root, "@extra"), "kind");
    int priority = 1;
    if (!std::strncmp(kind, "reaction_", 9) || !std::strncmp(kind, "sticker_", 8) || !std::strncmp(kind, "forum_", 6)) priority = 4;
    if (!std::strcmp(method, "getChatHistory") || !std::strcmp(method, "getForumTopicHistory") || !std::strcmp(method, "getForumTopics")) priority = 3;
    else if (!std::strcmp(method, "downloadFile")) priority = json_i53(root, "priority") >= 32 ? 4 : 1;
    else if (!std::strcmp(method, "cancelDownloadFile") || !std::strcmp(method, "sendMessage")) priority = 4;
    else if (!std::strncmp(kind, "refresh_main", 12)) priority = refresh_manual_ ? 3 : 0;
    else if (!std::strcmp(method, "loadChats")) priority = 2;
    else if (!std::strcmp(method, "optimizeStorage")) priority = 0;
    char* dumped = json_dumps(root, JSON_COMPACT | JSON_ENSURE_ASCII);
    if (dumped) {
        sendRaw(dumped, priority);
        std::free(dumped);
    }
    json_decref(root);
}

void TelegramClient::setInput(InputKind kind, const std::string& status) {
    input_kind_ = kind;
    status_ = status;
}

std::string TelegramClient::inputPrompt() const {
    switch (input_kind_) {
        case InputKind::Phone: return "Phone (+country...):";
        case InputKind::Code: return "Telegram login code:";
        case InputKind::Password: return "2FA password:";
        case InputKind::FirstName: return "First name:";
        case InputKind::Message: return "Message:";
        default: return "";
    }
}

void TelegramClient::cancelInput() {
    if (input_kind_ == InputKind::Message) input_kind_ = InputKind::None;
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
        status_ = "Sending login code...";
    } else if (kind == InputKind::Code) {
        root = json_pack("{s:s,s:s}", "@type", "checkAuthenticationCode", "code", value.c_str());
        status_ = "Checking code...";
    } else if (kind == InputKind::Password) {
        root = json_pack("{s:s,s:s}", "@type", "checkAuthenticationPassword", "password", value.c_str());
        status_ = "Checking password...";
    } else if (kind == InputKind::FirstName) {
        root = json_pack("{s:s,s:s,s:s}", "@type", "registerUser", "first_name", value.c_str(), "last_name", "");
        status_ = "Registering...";
    } else if (kind == InputKind::Message && current_chat_id_ != 0) {
        root = json_object();
        json_object_set_new(root, "@type", json_string("sendMessage"));
        json_object_set_new(root, "@extra", json_string("send_message"));
        json_object_set_new(root, "chat_id", json_integer(current_chat_id_));
        json_object_set_new(root, "message_thread_id", json_integer(0));
        json_object_set_new(root, "reply_to_message_id", json_integer(0));
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
        status_ = "Sending...";
    }

    if (root) sendJson(root);
}

void TelegramClient::processAuth(void* opaque) {
    json_t* root = (json_t*)opaque;
    json_t* state = json_object_get(root, "authorization_state");
    if (!json_is_object(state)) state = root;
    const char* type = json_str(state, "@type");

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
        json_object_set_new(params, "database_encryption_key", json_string(""));
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
        sendJson(params);
        status_ = "Initializing Telegram...";
    } else if (!std::strcmp(type, "authorizationStateWaitEncryptionKey")) {
        sendRaw("{\"@type\":\"checkDatabaseEncryptionKey\",\"encryption_key\":\"\"}");
    } else if (!std::strcmp(type, "authorizationStateWaitPhoneNumber")) {
        ready_ = false; known_chats_.clear(); chats_.clear(); leaveChat(); ui_cache_.clear(); user_names_.clear(); sender_requests_.clear();
        folder_windows_.clear(); folders_.clear(); selected_folder_ = 0; user_presence_.clear();
        setInput(InputKind::Phone, "Enter your phone number");
    } else if (!std::strcmp(type, "authorizationStateWaitCode")) {
        setInput(InputKind::Code, "Enter the login code sent by Telegram");
    } else if (!std::strcmp(type, "authorizationStateWaitPassword")) {
        setInput(InputKind::Password, "Two-step verification is enabled");
    } else if (!std::strcmp(type, "authorizationStateWaitRegistration")) {
        setInput(InputKind::FirstName, "Telegram needs a first name to finish registration");
    } else if (!std::strcmp(type, "authorizationStateWaitOtherDeviceConfirmation")) {
        status_ = std::string("Confirm login on another device: ") + json_str(state, "link");
    } else if (!std::strcmp(type, "authorizationStateReady")) {
        ready_ = true;
        error_.clear();
        status_ = "Online";
        input_kind_ = InputKind::None;
        requestChatsPage();
        if (current_chat_id_) { history_done_ = false; requestHistoryPage(); }
        next_update_check_ = sceKernelGetSystemTimeWide() + 10000000;
    } else if (!std::strcmp(type, "authorizationStateLoggingOut")) {
        ready_ = false; known_chats_.clear(); chats_.clear(); leaveChat(); ui_cache_.clear(); user_names_.clear(); sender_requests_.clear();
        folder_windows_.clear(); folders_.clear(); selected_folder_ = 0; user_presence_.clear();
    } else if (!std::strcmp(type, "authorizationStateClosed")) {
        closed_ = true;
        ready_ = false;
        status_ = "TDLib closed";
    }
}

bool TelegramClient::currentChatIsGroup() const {
    for (const auto& chat : known_chats_) if (chat.id == current_chat_id_) return chat.group;
    return false;
}

void TelegramClient::parseUser(void* opaque) {
    json_t* user = static_cast<json_t*>(opaque);
    int64_t id = json_i53(user, "id");
    if (!id) return;
    std::string name = json_str(user, "first_name");
    std::string last = json_str(user, "last_name");
    if (!last.empty()) { if (!name.empty()) name += " "; name += last; }
    user_names_[id] = name.empty() ? "Удалённый аккаунт" : name;
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
    if (found == user_presence_.end()) return "Статус загружается";
    const auto& p = found->second;
    if (p.bot) return "бот";
    if (p.type == "userStatusOnline") return ready_ && p.time > (int64_t)std::time(nullptr) ? "в сети" : "статус уточняется";
    if (p.type == "userStatusRecently") return "был(а) недавно";
    if (p.type == "userStatusLastWeek") return "был(а) на этой неделе";
    if (p.type == "userStatusLastMonth") return "был(а) в этом месяце";
    if (p.type == "userStatusOffline" && p.time > 0) {
        SceDateTime date{}; SceRtcTick utc{}, local{};
        if (sceRtcSetTime_t(&date, (time_t)p.time) >= 0 && sceRtcGetTick(&date, &utc) >= 0) {
            bool local_ok = sceRtcConvertUtcToLocalTime(&utc, &local) >= 0 && sceRtcSetTick(&date, &local) >= 0;
            char text[96];
            std::snprintf(text, sizeof(text), "был(а) %02u.%02u.%04u %02u:%02u%s", (unsigned)date.day,
                (unsigned)date.month, (unsigned)date.year, (unsigned)date.hour, (unsigned)date.minute, local_ok ? "" : " UTC");
            return text;
        }
    }
    return "статус недоступен";
}

std::string TelegramClient::senderName(const MessageItem& message) const {
    if (message.sender_chat) {
        for (const auto& chat : known_chats_) if (chat.id == message.sender_id) return chat.title;
    } else {
        auto it = user_names_.find(message.sender_id);
        if (it != user_names_.end()) return it->second;
    }
    return "Участник";
}

void TelegramClient::parseChat(void* opaque) {
    json_t* chat = (json_t*)opaque;
    ChatItem item;
    json_t* chat_type = json_object_get(chat, "type");
    item.supergroup_id = json_i53(chat_type, "supergroup_id");
    if (!std::strcmp(json_str(chat_type, "@type"), "chatTypePrivate") || !std::strcmp(json_str(chat_type, "@type"), "chatTypeSecret"))
        item.user_id = json_i53(chat_type, "user_id");
    item.group = !std::strcmp(json_str(chat_type, "@type"), "chatTypeBasicGroup") ||
        (!std::strcmp(json_str(chat_type, "@type"), "chatTypeSupergroup") && !json_is_true(json_object_get(chat_type, "is_channel")));
    item.id = json_i53(chat, "id");
    if (!item.id) return;
    updateReadOutbox(item.id, json_i53(chat, "last_read_outbox_message_id"));

    // Retain metadata across tabs; membership/order belongs to each TDLib list.
    json_t* positions = json_object_get(chat, "positions");
    if (json_is_array(positions)) {
        size_t position_index; json_t* position;
        json_array_foreach(positions, position_index, position) {
            json_t* list = json_is_object(position) ? json_object_get(position, "list") : nullptr;
            int32_t list_id = chat_list_id(list);
            if (list_id >= 0 && json_i53(position, "order")) item.list_orders[list_id] = json_i53(position, "order");
        }
    }
    item.title = json_str(chat, "title", "(untitled)");
    item.last_message_id = json_i53(json_object_get(chat, "last_message"), "id");
    json_t* photo = json_object_get(chat, "photo");
    json_t* small = json_is_object(photo) ? json_object_get(photo, "small") : nullptr;
    if (json_is_object(small)) {
        item.avatar_file_id = (int32_t)json_i53(small, "id");
        json_t* local = json_object_get(small, "local");
        if (json_is_object(local) && json_is_true(json_object_get(local, "is_downloading_completed"))) item.avatar_path = json_str(local, "path");
    }
    auto it = std::find_if(known_chats_.begin(), known_chats_.end(), [&](const ChatItem& c){ return c.id == item.id; });
    if (it != known_chats_.end()) {
        noteLastMessage(item.id, json_object_get(chat, "last_message"));
        item.last_message_id = std::max(item.last_message_id, it->last_message_id);
    }
    if (it == known_chats_.end()) known_chats_.push_back(item);
    else *it = item;
}

static void parse_reactions(MessageItem& message, json_t* info) {
    message.reactions.clear();
    json_t* reactions = json_object_get(json_object_get(info, "reactions"), "reactions");
    size_t i; json_t* reaction;
    json_array_foreach(reactions, i, reaction) {
        json_t* type = json_object_get(reaction, "type");
        const char* kind = json_str(type, "@type");
        std::string emoji = !std::strcmp(kind, "reactionTypeEmoji") ? json_str(type, "emoji") :
            !std::strcmp(kind, "reactionTypePaid") ? "Stars" : "Custom";
        message.reactions.push_back({emoji, (int)json_i53(reaction, "total_count"), json_is_true(json_object_get(reaction, "is_chosen"))});
    }
}

MessageItem TelegramClient::parseMessage(void* opaque) {
    json_t* msg = (json_t*)opaque;
    MessageItem out;
    parse_reactions(out, json_object_get(msg, "interaction_info"));
    json_t* sender = json_object_get(msg, "sender_id");
    out.sender_chat = !std::strcmp(json_str(sender, "@type"), "messageSenderChat");
    out.sender_id = json_i53(sender, out.sender_chat ? "chat_id" : "user_id");
    out.id = json_i53(msg, "id");
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
    if (!std::strcmp(type, "messageText")) {
        json_t* ft = json_object_get(content, "text");
        out.text = json_is_object(ft) ? json_str(ft, "text", "") : "";
    } else if (!std::strcmp(type, "messageAnimation") || !std::strcmp(type, "messageVideo") || !std::strcmp(type, "messageVideoNote")) {
        out.animation = true;
        out.video = std::strcmp(type, "messageAnimation") != 0;
        out.video_note = !std::strcmp(type, "messageVideoNote");
        out.text = std::string(out.video_note ? "[video note] " : out.video ? "[video] " : "[GIF] ") + json_str(json_object_get(content, "caption"), "text");
        json_t* animation = json_object_get(content, out.video_note ? "video_note" : out.video ? "video" : "animation");
        out.video_duration = std::max<int64_t>(0, json_i53(animation, "duration"));
        json_t* file = json_object_get(animation, out.video ? "video" : "animation");
        out.animation_file_id = (int32_t)json_i53(file, "id");
        out.animation_size = std::max(json_i53(file, "size"), json_i53(file, "expected_size"));
        json_t* local = json_object_get(file, "local");
        out.animation_downloaded = json_i53(local, "downloaded_size");
        if (json_is_true(json_object_get(local, "is_downloading_completed"))) out.animation_path = json_str(local, "path");
        int64_t w = json_i53(animation, "width"), h = json_i53(animation, "height");
        if (out.video_note) w = h = json_i53(animation, "length");
        if (out.animation_size > (out.video ? 128 : 20) * 1024 * 1024) out.animation_error = out.video ? "Video limit: 128 MiB" : "GIF limit: 20 MiB";
        else if (w > 1280 || h > 1280 || (w > 0 && h > 0 && w * h > 1280 * 720))
            out.animation_error = "Video/animation exceeds 720p pixel limit";
        else if (json_is_true(json_object_get(content, "is_secret"))) out.animation_error = "Self-destructing media is not supported";
        json_t* thumb = json_object_get(animation, "thumbnail");
        const char* format = json_str(json_object_get(thumb, "format"), "@type");
        if (!std::strcmp(format, "thumbnailFormatJpeg") || !std::strcmp(format, "thumbnailFormatPng") || !std::strcmp(format, "thumbnailFormatWebp")) {
            json_t* preview = json_object_get(thumb, "file");
            out.media_file_id = (int32_t)json_i53(preview, "id");
            json_t* preview_local = json_object_get(preview, "local");
            if (json_is_true(json_object_get(preview_local, "is_downloading_completed"))) out.media_path = json_str(preview_local, "path");
        }
    } else if (!std::strcmp(type, "messageAnimatedEmoji")) {
        // Render the Unicode fallback without downloading animation or sound.
        out.text = json_str(content, "emoji");
    } else if (!std::strcmp(type, "messagePhoto")) {
        json_t* caption = json_object_get(content, "caption");
        const char* caption_text = json_is_object(caption) ? json_str(caption, "text") : "";
        out.text = caption_text[0] ? caption_text : "[photo]";
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
        out.text = std::string(json_str(sticker, "emoji")) + (animated ? " [animated sticker]" : " [sticker]");
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
        json_t* file = json_object_get(note, out.audio ? "audio" : "voice");
        if (out.audio) {
            out.audio_title = json_str(note, "title");
            if (out.audio_title.empty()) out.audio_title = json_str(note, "file_name");
            if (out.audio_title.empty()) out.audio_title = "Audio";
            out.audio_performer = json_str(note, "performer");
        }
        json_t* local = json_object_get(file, "local");
        out.voice_duration = std::max<int64_t>(0, json_i53(note, "duration"));
        out.voice_file_id = (int32_t)json_i53(file, "id");
        out.voice_size = std::max(json_i53(file, "size"), json_i53(file, "expected_size"));
        out.voice_downloaded = json_i53(local, "downloaded_size");
        if (json_is_true(json_object_get(local, "is_downloading_completed"))) out.voice_path = json_str(local, "path");
        if (out.voice_size > (out.audio ? 128 : 20) * 1024 * 1024) out.voice_error = out.audio ? "Audio limit: 128 MiB" : "Voice limit: 20 MiB";
        out.text = out.audio ? "[audio] " + out.audio_title + (out.audio_performer.empty() ? "" : " - " + out.audio_performer)
            : "[voice] " + std::to_string(out.voice_duration) + " s";
        std::string caption = json_str(json_object_get(content, "caption"), "text");
        if (!caption.empty()) out.text += "\n" + caption;
    }
    else if (!std::strcmp(type, "messageDocument")) out.text = "[file]";
    else out.text = type[0] ? std::string("[") + type + "]" : "[message]";
    auto add_link = [&](const std::string& url) {
        std::string prefix = url.substr(0, 8);
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (prefix.compare(0, 7, "http://") && prefix != "https://") return;
        if (url.find_first_of("\r\n\t ") != std::string::npos) return;
        if (std::find(out.links.begin(), out.links.end(), url) == out.links.end()) out.links.push_back(url);
    };
    std::string lower = out.text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (size_t pos = 0; (pos = lower.find("http", pos)) != std::string::npos;) {
        size_t end = out.text.find_first_of(" \t\r\n<>\"", pos);
        if (end == std::string::npos) end = out.text.size();
        std::string url = out.text.substr(pos, end - pos);
        while (!url.empty() && std::string(".,;!").find(url.back()) != std::string::npos) url.pop_back();
        while (!url.empty() && url.back() == ')' && std::count(url.begin(), url.end(), ')') > std::count(url.begin(), url.end(), '(')) url.pop_back();
        add_link(url);
        pos = end;
    }
    json_t* formatted = json_object_get(content, !std::strcmp(type, "messageText") ? "text" : "caption");
    json_t* entities = json_object_get(formatted, "entities");
    if (json_is_array(entities)) {
        size_t i; json_t* entity;
        json_array_foreach(entities, i, entity) {
            json_t* entity_type = json_object_get(entity, "type");
            if (!std::strcmp(json_str(entity_type, "@type"), "textEntityTypeTextUrl")) add_link(json_str(entity_type, "url"));
        }
    }
    return out;
}

void TelegramClient::requestFile(int32_t file_id, int priority) {
    if (!ready_) return;
    if (file_id <= 0) return;
    bool fresh = pending_files_.insert(file_id).second;
    if (!fresh && file_priorities_[file_id] >= priority) return;
    file_priorities_[file_id] = priority;
    json_t* req = json_pack("{s:s,s:i,s:i,s:I,s:I,s:b}",
        "@type", "downloadFile", "file_id", file_id, "priority", priority,
        "offset", (json_int_t)0, "limit", (json_int_t)0, "synchronous", 0);
    sendJson(req);
}

void TelegramClient::updateFile(void* opaque) {
    json_t* file = (json_t*)opaque;
    int32_t id = (int32_t)json_i53(file, "id");
    json_t* local = json_object_get(file, "local");
    const char* path = json_is_object(local) ? json_str(local, "path") : "";
    if (id <= 0) return;
    if (json_is_true(json_object_get(local, "is_downloading_completed")))
        for (auto& sticker : stickers_) if (sticker.preview == id) sticker.path = path;
    auto update_voice = [&](MessageItem& message) {
        if (message.voice_file_id != id) return;
        message.voice_size = std::max(json_i53(file, "size"), json_i53(file, "expected_size"));
        message.voice_downloaded = json_i53(local, "downloaded_size");
        if (std::max(message.voice_size, message.voice_downloaded) > (message.audio ? 128 : 20) * 1024 * 1024)
            message.voice_error = message.audio ? "Audio limit: 128 MiB" : "Voice limit: 20 MiB";
        if (json_is_true(json_object_get(local, "is_downloading_completed"))) message.voice_path = path;
    };
    for (auto& message : messages_) update_voice(message);
    for (auto& message : history_items_) update_voice(message);
    auto update_animation = [&](MessageItem& message) {
        if (message.animation_file_id != id) return;
        message.animation_size = std::max(json_i53(file, "size"), json_i53(file, "expected_size"));
        message.animation_downloaded = json_i53(local, "downloaded_size");
        if (std::max(message.animation_size, message.animation_downloaded) > (message.video ? 128 : 20) * 1024 * 1024)
            message.animation_error = message.video ? "Video limit: 128 MiB" : "GIF limit: 20 MiB";
        if (json_is_true(json_object_get(local, "is_downloading_completed"))) message.animation_path = path;
    };
    for (auto& message : messages_) update_animation(message);
    for (auto& message : history_items_) update_animation(message);
    if (!json_is_true(json_object_get(local, "is_downloading_completed"))) {
        for (auto& chat : known_chats_) if (chat.avatar_file_id == id) chat.avatar_path.clear();
        for (auto& chat : chats_) if (chat.avatar_file_id == id) chat.avatar_path.clear();
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
    for (auto& chat : chats_) if (chat.avatar_file_id == id) chat.avatar_path = path;
    for (auto& chat : known_chats_) if (chat.avatar_file_id == id) chat.avatar_path = path;
    for (auto& message : messages_) {
        if (message.media_file_id == id) message.media_path = path;
        for (auto& photo : message.photos) {
            if (photo.thumbnail_file_id == id) photo.thumbnail_path = path;
            if (photo.full_file_id == id) photo.full_path = path;
        }
    }
    for (auto& message : history_items_) {
        if (message.media_file_id == id) message.media_path = path;
        for (auto& photo : message.photos) {
            if (photo.thumbnail_file_id == id) photo.thumbnail_path = path;
            if (photo.full_file_id == id) photo.full_path = path;
        }
    }
}

void TelegramClient::ensureChatMedia(size_t index) {
    if (index >= chats_.size()) return;
    const auto& chat = chats_[index];
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
    if (chat.avatar_path.empty()) requestFile(chat.avatar_file_id, 8);
}

void TelegramClient::ensureMessageMedia(size_t index) {
    if (index >= messages_.size()) return;
    const auto& message = messages_[index];
    if (ready_ && currentChatIsGroup() && !message.outgoing && message.sender_id && senderName(message) == "Участник") {
        std::string key = std::string(message.sender_chat ? "c" : "u") + std::to_string(message.sender_id);
        if (sender_requests_.insert(key).second)
            sendJson(json_pack("{s:s,s:I,s:s}", "@type", message.sender_chat ? "getChat" : "getUser",
                message.sender_chat ? "chat_id" : "user_id", (json_int_t)message.sender_id, "@extra", "sender_lookup"));
    }
    if (!message.photos.empty()) {
        ensurePhotoThumbnail(index, 0);
    } else if (message.media_path.empty()) requestFile(message.media_file_id, 16);
}

void TelegramClient::setAnimationTarget(int index) {
    const MessageItem* message = index >= 0 && index < (int)messages_.size() ? &messages_[index] : nullptr;
    int32_t wanted = message && message->animation && message->animation_error.empty() ? message->animation_file_id : 0;
    if (wanted != active_animation_file_) {
        if (active_animation_file_ && pending_files_.erase(active_animation_file_))
            sendJson(json_pack("{s:s,s:i,s:b}", "@type", "cancelDownloadFile", "file_id", active_animation_file_, "only_if_pending", 0));
        active_animation_file_ = wanted;
    }
    if (wanted && message->animation_path.empty() && pending_files_.insert(wanted).second) {
        ++animation_download_generation_;
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
    json_t* root = static_cast<json_t*>(opaque);
    if (!root) return;
    media_cache_.resolve(root);
    ui_cache_.remember(root);

    const char* type = json_str(root, "@type");
    if (!std::strcmp(type, "updateUserStatus")) {
        parseUserStatus(json_i53(root, "user_id"), json_object_get(root, "status"));
        json_decref(root); return;
    }
    if (!std::strcmp(json_str(root, "@extra"), "cache_cleanup")) {
        cache_maintenance_ = false;
        if (!std::strcmp(type, "error")) error_ = std::string("Cache: ") + json_str(root, "message");
        json_decref(root); return;
    }
    if (!std::strcmp(type, "user") || !std::strcmp(type, "updateUser")) {
        parseUser(!std::strcmp(type, "user") ? root : json_object_get(root, "user"));
        json_decref(root); return;
    }
    if (!std::strcmp(type, "error") && !std::strcmp(json_str(root, "@extra"), "sender_lookup")) {
        json_decref(root); return;
    }
    json_t* extra = json_object_get(root, "@extra");
    if (!std::strcmp(json_str(extra, "kind"), "forum_check") || !std::strcmp(json_str(extra, "kind"), "forum_topics")) {
        if (json_i53(extra, "chat") == current_chat_id_ && json_i53(extra, "generation") == topics_generation_) {
            topics_pending_ = false;
            if (!std::strcmp(type, "error")) topics_status_ = json_str(root, "message");
            else if (!std::strcmp(json_str(extra, "kind"), "forum_check")) {
                if (json_is_true(json_object_get(root, "is_forum"))) loadTopics(true);
                else { topics_open_ = false; refreshHistory(); }
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
                topics_status_ = topics_.empty() ? "Нет доступных тем" : topics_end_ ? "Все темы загружены" : "Вниз / Triangle: загрузить ещё темы";
            }
        }
        json_decref(root); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "sticker_packs") ||
        !std::strcmp(json_str(extra, "kind"), "sticker_items")) {
        bool packs = !std::strcmp(json_str(extra, "kind"), "sticker_packs");
        if (json_i53(extra, "chat") == current_chat_id_ &&
            json_i53(extra, "session") == sticker_session_ &&
            (packs || json_i53(extra, "generation") == sticker_generation_)) {
            if (!std::strcmp(type, "error")) sticker_status_ = json_str(root, "message");
            else if (packs) {
                json_t* sets = json_object_get(root, "sets"); size_t i; json_t* set;
                json_array_foreach(sets, i, set) {
                    sticker_packs_.push_back({(int32_t)sticker_packs_.size(), json_str(set, "title")});
                    sticker_set_ids_.push_back(json_i53(set, "id"));
                }
            } else {
                stickers_.clear(); json_t* items = json_object_get(root, "stickers"); size_t i; json_t* item;
                json_array_foreach(items, i, item) {
                    SendStickerItem s;
                    s.file = (int32_t)json_i53(json_object_get(item, "sticker"), "id");
                    s.width = (int)json_i53(item, "width"); s.height = (int)json_i53(item, "height");
                    s.emoji = json_str(item, "emoji");
                    json_t* preview = json_object_get(json_object_get(item, "thumbnail"), "file");
                    if (!std::strcmp(json_str(json_object_get(item, "format"), "@type"), "stickerFormatWebp"))
                        preview = json_object_get(item, "sticker");
                    s.preview = (int32_t)json_i53(preview, "id");
                    json_t* local = json_object_get(preview, "local");
                    if (json_is_true(json_object_get(local, "is_downloading_completed"))) s.path = json_str(local, "path");
                    if (s.file) stickers_.push_back(s);
                }
                sticker_status_ = stickers_.empty() ? "В этом разделе нет стикеров" : "X: отправить выбранный стикер";
            }
        }
        json_decref(root); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "reaction_choices") || !std::strcmp(json_str(extra, "kind"), "reaction_send")) {
        if (json_i53(extra, "chat") == current_chat_id_ && json_i53(extra, "message") == reaction_message_) {
            reaction_pending_ = false;
            if (!std::strcmp(type, "error")) reaction_status_ = json_str(root, "message");
            else if (!std::strcmp(type, "availableReactions")) {
                reaction_choices_.clear();
                for (const char* field : {"top_reactions", "recent_reactions", "popular_reactions"}) {
                    size_t i; json_t* option;
                    json_array_foreach(json_object_get(root, field), i, option) {
                        json_t* kind = json_object_get(option, "type");
                        if (std::strcmp(json_str(kind, "@type"), "reactionTypeEmoji") || json_is_true(json_object_get(option, "needs_premium"))) continue;
                        std::string emoji = json_str(kind, "emoji");
                        if (!emoji.empty() && std::find(reaction_choices_.begin(), reaction_choices_.end(), emoji) == reaction_choices_.end()) reaction_choices_.push_back(emoji);
                    }
                }
                for (const auto& message : messages_) if (message.id == reaction_message_)
                    for (const auto& reaction : message.reactions)
                        if (reaction.chosen && reaction.emoji != "Stars" && reaction.emoji != "Custom" &&
                            std::find(reaction_choices_.begin(), reaction_choices_.end(), reaction.emoji) == reaction_choices_.end())
                            reaction_choices_.push_back(reaction.emoji);
                reaction_status_ = reaction_choices_.empty() ? "Нет доступных обычных реакций" : "X: поставить / убрать";
            } else {
                reaction_status_ = "Реакция обновлена";
                sendJson(json_pack("{s:s,s:I,s:I}", "@type", "getMessage", "chat_id", (json_int_t)current_chat_id_, "message_id", (json_int_t)reaction_message_));
            }
        }
        json_decref(root); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "voice_download") && !std::strcmp(type, "error")) {
        int32_t file = (int32_t)json_i53(extra, "file");
        if (file == active_voice_file_ && json_i53(extra, "generation") == voice_download_generation_) {
            pending_files_.erase(file);
            for (auto& message : history_items_) if (message.voice_file_id == file)
                message.voice_error = std::string("Audio download: ") + json_str(root, "message");
            rebuildHistory();
        }
        json_decref(root); return;
    }
    const char* list_kind = json_is_object(extra) ? json_str(extra, "kind") : json_str(root, "@extra");
    if (!std::strcmp(list_kind, "avatar_metadata") && !std::strcmp(type, "error")) {
        // Retry only while the row is visible, with the existing 10s backoff.
        json_decref(root); return;
    }
    bool list_reply = !std::strcmp(list_kind, "refresh_main_ids") || !std::strcmp(list_kind, "refresh_main_chat") ||
                      !std::strcmp(list_kind, "load_main_chats");
    if (list_reply && json_i53(extra, "list_generation") != list_generation_) {
        // Metadata remains useful after switching; request state does not.
        if (!std::strcmp(type, "chat")) parseChat(root);
        json_decref(root); return;
    }
    if (!std::strcmp(json_str(extra, "kind"), "animation_download") && !std::strcmp(type, "error")) {
        int32_t file = (int32_t)json_i53(extra, "file");
        if (file == active_animation_file_ && json_i53(extra, "generation") == animation_download_generation_) {
            pending_files_.erase(file);
            for (auto& message : history_items_) if (message.animation_file_id == file && message.animation_path.empty())
                message.animation_error = std::string("Media download: ") + json_str(root, "message");
            rebuildHistory();
        }
        json_decref(root); return;
    }
    if (!std::strcmp(list_kind, "refresh_main_ids")) {
        if (!std::strcmp(type, "error")) {
            chats_error_ = json_str(root, "message");
            refresh_pending_ = false;
            notice_ = "Refresh failed"; notice_until_ = sceKernelGetSystemTimeWide() + 8000000;
        } else {
            json_t* ids = json_object_get(root, "chat_ids");
            size_t index; json_t* id;
            if (json_is_array(ids)) json_array_foreach(ids, index, id) {
                int64_t chat_id = json_integer_value(id);
                auto known = std::find_if(known_chats_.begin(), known_chats_.end(),
                    [&](const ChatItem& chat) { return chat.id == chat_id; });
                // Live TDLib updates already maintain known chat metadata. Refresh
                // cached snapshots once; avoid 15 redundant getChat calls every 10s.
                if ((known == known_chats_.end() || known->cached_metadata) && refresh_ids_.insert(chat_id).second) {
                    sendListRequest("getChat", "refresh_main_chat", chat_id);
                }
            }
            if (refresh_ids_.empty()) finishChatRefresh();
        }
        json_decref(root); return;
    }
    bool refresh_chat = !std::strcmp(list_kind, "refresh_main_chat");
    if (refresh_chat && !std::strcmp(type, "error")) {
        chats_error_ = json_str(root, "message");
        refresh_ids_.clear(); refresh_pending_ = false;
        notice_ = "Refresh failed"; notice_until_ = sceKernelGetSystemTimeWide() + 8000000;
        json_decref(root); return;
    }
    if (!std::strcmp(list_kind, "load_main_chats")) {
        chats_pending_ = false;
        next_chats_request_ = sceKernelGetSystemTimeWide() + 500000;
        if (!std::strcmp(type, "error")) {
            if (json_i53(root, "code") == 404) chats_end_ = true;
            else chats_error_ = json_str(root, "message");
        }
        json_decref(root);
        return;
    }
    bool history_reply = json_is_object(extra) && !std::strcmp(json_str(extra, "kind"), "history");
    if (history_reply && (json_i53(extra, "chat") != current_chat_id_ ||
        json_i53(extra, "generation") != history_generation_)) { json_decref(root); return; }
    if (history_reply && !std::strcmp(type, "error")) {
        history_pending_ = false;
        history_error_ = json_str(root, "message");
    }
    if (!std::strcmp(type, "updateChatFolders")) {
        parseFolders(root);
    } else if (!std::strcmp(type, "updateAuthorizationState") ||
        !std::strncmp(type, "authorizationState", 18)) {
        processAuth(root);
    } else if (!std::strcmp(type, "error")) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Telegram error %lld: %s", (long long)json_i53(root, "code"), json_str(root, "message"));
        error_ = buf;
        status_ = error_;
        if (!std::strcmp(json_str(root, "@extra"), "send_message")) {
            notice_ = error_; notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
        }
    } else if (!std::strcmp(type, "message")) {
        storeMessage(root);
    } else if (!std::strcmp(type, "updateMessageSendSucceeded") || !std::strcmp(type, "updateMessageSendFailed")) {
        json_t* message = json_object_get(root, "message");
        storeMessage(message, json_i53(root, "old_message_id"));
        if (!std::strcmp(type, "updateMessageSendFailed") && json_i53(message, "chat_id") == current_chat_id_) {
            notice_ = "Message failed to send"; notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
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
            if (history_cached_) { history_items_.clear(); history_cached_ = false; }
            history_pending_ = false;
            int64_t old_cursor = history_cursor_;
            size_t i; json_t* m;
            json_array_foreach(arr, i, m) {
                if (json_is_object(m) && matchesTopic(m)) {
                    MessageItem parsed = parseMessage(m);
                    if (replaced_message_ids_.count(parsed.id)) continue;
                    if (!history_cursor_ || parsed.id < history_cursor_) history_cursor_ = parsed.id;
                    if (std::none_of(history_items_.begin(), history_items_.end(), [&](const MessageItem& v) { return v.id == parsed.id; }))
                        history_items_.push_back(std::move(parsed));
                }
            }
            history_end_ = json_array_size(arr) == 0;
            if (!history_end_ && old_cursor && old_cursor == history_cursor_)
                history_error_ = "No older messages returned. Swipe up to retry.";
            history_done_ = history_end_ || (int)history_items_.size() >= history_target_;
            rebuildHistory();
            status_ = history_done_ ? "Online" : "Loading older messages...";
        }
    } else if (!std::strcmp(type, "updateNewChat")) {
        json_t* chat = json_object_get(root, "chat");
        if (json_is_object(chat)) parseChat(chat);
    } else if (!std::strcmp(type, "updateChatLastMessage")) {
        int64_t id = json_i53(root, "chat_id");
        updateChatPositions(id, json_object_get(root, "positions"));
        noteLastMessage(id, json_object_get(root, "last_message"));
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
        if (list_id < 0) { json_decref(root); return; }
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
    } else if (!std::strcmp(type, "updateMessageInteractionInfo")) {
        if (json_i53(root, "chat_id") == current_chat_id_) {
            for (auto& message : history_items_) if (message.id == json_i53(root, "message_id"))
                parse_reactions(message, json_object_get(root, "interaction_info"));
            rebuildHistory();
        }
    } else if (!std::strcmp(type, "updateMessageEdited")) {
        if (json_i53(root, "chat_id") == current_chat_id_) {
            for (auto& item : history_items_) if (item.id == json_i53(root, "message_id"))
                item.edit_date = json_i53(root, "edit_date");
            rebuildHistory();
        }
    } else if (!std::strcmp(type, "updateFile")) {
        json_t* file = json_object_get(root, "file");
        if (json_is_object(file)) updateFile(file);
    } else if (!std::strcmp(type, "file")) {
        updateFile(root);
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

    json_decref(root);
}

void TelegramClient::pump() {
    if (!worker_started_) return;
    int64_t pump_start = sceKernelGetSystemTimeWide();
    bool changed = false;
    for (int i = 0; i < 32; ++i) {
        void* msg = nullptr;
        pthread_mutex_lock(&queue_mutex_);
        if (!parsed_.empty()) { msg = parsed_.front(); parsed_.pop_front(); }
        pthread_mutex_unlock(&queue_mutex_);
        if (!msg) break;
        process(msg);
        changed = true;
        if (sceKernelGetSystemTimeWide() - pump_start >= 2000) break;
    }
    if (!history_pending_ && !history_done_ && history_error_.empty()) requestHistoryPage();
    if (changed) publishChats();
    if (ready_ && !current_chat_id_ && !chats_pending_ && !chats_end_ && chats_error_.empty() &&
        chats_.size() < chats_target_ && sceKernelGetSystemTimeWide() >= next_chats_request_) requestChatsPage();
    if (ready_ && !current_chat_id_ && !refresh_pending_ && sceKernelGetSystemTimeWide() >= next_update_check_)
        checkChatUpdates(false);
}

void TelegramClient::publishChats() {
    // TDLib can send cached updates beyond loadChats.limit. Expose only the
    // requested window; never download avatars for the undisplayed remainder.
    chats_.clear();
    for (const auto& chat : known_chats_) {
        auto order = chat.list_orders.find(selected_folder_);
        if (order == chat.list_orders.end() || !order->second) continue;
        chats_.push_back(chat); chats_.back().order = order->second;
    }
    std::sort(chats_.begin(), chats_.end(), [](const ChatItem& a, const ChatItem& b) {
        return a.order != b.order ? a.order > b.order : a.id > b.id;
    });
    selected_chat_count_ = chats_.size();
    if (chats_.size() > chats_target_) chats_.resize(chats_target_);
}

void TelegramClient::sendListRequest(const char* method, const char* kind, int64_t chat_id) {
    json_t* request = json_pack("{s:s}", "@type", method);
    if (chat_id) json_object_set_new(request, "chat_id", json_integer(chat_id));
    else {
        json_object_set_new(request, "chat_list", selected_folder_ ?
            json_pack("{s:s,s:i}", "@type", "chatListFolder", "chat_folder_id", selected_folder_) :
            json_pack("{s:s}", "@type", "chatListMain"));
        json_object_set_new(request, "limit", json_integer(15));
    }
    json_object_set_new(request, "@extra", json_pack("{s:s,s:I}", "kind", kind,
        "list_generation", (json_int_t)list_generation_));
    sendJson(request);
}

void TelegramClient::selectFolder(int32_t id) {
    if (id == selected_folder_) return;
    if (id && std::none_of(folders_.begin(), folders_.end(), [&](const ChatFolderItem& f) { return f.id == id; })) return;
    folder_windows_[selected_folder_] = {chats_target_, chats_end_};
    selected_folder_ = id; ++list_generation_;
    auto saved = folder_windows_.find(id);
    chats_target_ = saved == folder_windows_.end() ? 15 : saved->second.target;
    chats_pending_ = false;
    chats_end_ = saved != folder_windows_.end() && saved->second.end;
    refresh_pending_ = refresh_manual_ = false; refresh_ids_.clear();
    chats_error_.clear(); notice_.clear(); notice_until_ = 0;
    next_chats_request_ = 0; next_update_check_ = 0;
    refresh_top_ = true;
    publishChats();
    if (saved == folder_windows_.end() || chats_.size() < chats_target_) requestChatsPage();
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
        if (title.empty()) title = json_str(item, "title", "Folder");
        folders_.push_back({id, title});
    }
    if (!folders_.empty()) {
        int position = std::max(0, std::min((int)folders_.size(), (int)json_i53(update, "main_chat_list_position")));
        folders_.insert(folders_.begin() + position, {0, "Все чаты"});
    }
    if (selected_folder_ && std::none_of(folders_.begin(), folders_.end(), [&](const ChatFolderItem& f) { return f.id == selected_folder_; })) {
        // A folder may be removed remotely while its chat is open.
        selectFolder(0);
    }
}

void TelegramClient::rebuildHistory() {
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
            if ((album.text.empty() || album.text == "[photo]") && item.text != "[photo]") album.text = item.text;
        } else messages_.push_back(item);
    }
}

void TelegramClient::refreshChats() {
    checkChatUpdates(true);
}

std::string TelegramClient::updateNotice() const {
    return sceKernelGetSystemTimeWide() < notice_until_ ? notice_ : "";
}

void TelegramClient::checkChatUpdates(bool manual) {
    if (!ready_) return;
    if (refresh_pending_) {
        if (manual) {
            refresh_manual_ = true;
            pthread_mutex_lock(&queue_mutex_);
            for (auto& request : outgoing_)
                if (request.text.find("\"kind\":\"refresh_main") != std::string::npos) request.priority = 3;
            pthread_mutex_unlock(&queue_mutex_);
            notice_ = "Refreshing chats...";
            notice_until_ = sceKernelGetSystemTimeWide() + 10000000;
        }
        return;
    }
    refresh_pending_ = true; refresh_manual_ = manual;
    refresh_before_.clear();
    for (const auto& chat : chats_) refresh_before_.push_back(chat.id);
    refresh_ids_.clear(); chats_error_.clear();
    next_update_check_ = sceKernelGetSystemTimeWide() + 10000000;
    if (manual) { notice_ = "Refreshing chats..."; notice_until_ = sceKernelGetSystemTimeWide() + 10000000; }
    // Snapshot the latest top entries; loadChats advances the older-page cursor.
    sendListRequest("getChats", "refresh_main_ids");
    if (chats_.empty() && !chats_pending_ && !chats_end_) requestChatsPage();
}

void TelegramClient::finishChatRefresh() {
    refresh_pending_ = false;
    publishChats();
    if (refresh_manual_) refresh_top_ = true;
    std::vector<int64_t> current;
    for (const auto& chat : chats_) current.push_back(chat.id);
    if ((refresh_manual_ || current != refresh_before_) &&
        (sceKernelGetSystemTimeWide() >= notice_until_ || notice_.compare(0, 13, "New message: ") != 0)) {
        notice_ = "Chat list updated";
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
        if (key >= 0 && json_i53(position, "order")) found->list_orders[key] = json_i53(position, "order");
    }
}

void TelegramClient::noteLastMessage(int64_t id, void* opaque) {
    json_t* message = static_cast<json_t*>(opaque);
    if (!json_is_object(message)) return;
    for (auto& chat : known_chats_) if (chat.id == id) {
        int64_t latest = json_i53(message, "id");
        if (latest > chat.last_message_id) {
            if (chat.list_orders.count(selected_folder_) && chat.last_message_id && !json_is_true(json_object_get(message, "is_outgoing"))) {
                notice_ = "New message: " + chat.title;
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
    sendListRequest("loadChats", "load_main_chats");
    status_ = "Loading chats...";
}

void TelegramClient::openChat(int64_t id, const std::string& title) {
    current_topic_ = 0; topics_open_ = topics_pending_ = false; topics_.clear(); ++topics_generation_;
    forum_title_ = title;
    replaced_message_ids_.clear();
    current_chat_id_ = id;
    current_chat_title_ = title;
    if (ready_) for (const auto& chat : known_chats_) if (chat.id == id && chat.user_id) {
        sendJson(json_pack("{s:s,s:I,s:s}", "@type", "getUser", "user_id", (json_int_t)chat.user_id, "@extra", "sender_lookup"));
        break;
    }
    if (ready_) for (const auto& chat : known_chats_) if (chat.id == id && chat.group && chat.supergroup_id) {
        topics_open_ = topics_pending_ = true; topics_status_ = "Проверка тем группы...";
        ++history_generation_; history_pending_ = false; history_done_ = true; history_items_.clear(); messages_.clear();
        json_t* req = json_pack("{s:s,s:I}", "@type", "getSupergroup", "supergroup_id", (json_int_t)chat.supergroup_id);
        json_object_set_new(req, "@extra", json_pack("{s:s,s:I,s:i}", "kind", "forum_check", "chat", (json_int_t)id, "generation", (int)topics_generation_));
        sendJson(req); return;
    }
    refreshHistory();
}

bool TelegramClient::matchesTopic(void* opaque) const {
    if (topics_open_) return false;
    return !current_topic_ || json_i53(json_object_get((json_t*)opaque, "topic_id"), "forum_topic_id") == current_topic_;
}

void TelegramClient::loadTopics(bool refresh) {
    if (!ready_ || !current_chat_id_ || topics_pending_) return;
    if (refresh) { ++topics_generation_; topics_.clear(); topics_date_ = topics_offset_ = 0; topics_message_ = 0; topics_end_ = false; }
    if (topics_end_) return;
    topics_open_ = topics_pending_ = true; topics_status_ = "Загрузка тем...";
    json_t* req = json_pack("{s:s,s:I,s:s,s:i,s:I,s:i,s:i}", "@type", "getForumTopics", "chat_id", (json_int_t)current_chat_id_, "query", "", "offset_date", topics_date_, "offset_message_id", (json_int_t)topics_message_, "offset_forum_topic_id", topics_offset_, "limit", 15);
    json_object_set_new(req, "@extra", json_pack("{s:s,s:I,s:i}", "kind", "forum_topics", "chat", (json_int_t)current_chat_id_, "generation", (int)topics_generation_));
    sendJson(req);
}

void TelegramClient::openTopic(size_t index) {
    if (index >= topics_.size()) return;
    current_topic_ = topics_[index].id; topics_open_ = false;
    current_chat_title_ = forum_title_ + " / " + topics_[index].title;
    refreshHistory();
}

void TelegramClient::backToTopics() {
    current_topic_ = 0; topics_open_ = true; current_chat_title_ = forum_title_;
    ++history_generation_; history_pending_ = false; history_done_ = true; history_items_.clear(); messages_.clear();
}

void TelegramClient::storeMessage(void* opaque, int64_t old_id) {
    json_t* message = static_cast<json_t*>(opaque);
    if (!json_is_object(message) || json_i53(message, "chat_id") != current_chat_id_ || !current_chat_id_) return;
    if (!matchesTopic(message)) return;
    MessageItem parsed = parseMessage(message);
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
    history_items_.clear(); messages_.clear();
    json_t* cached = ui_cache_.history(current_chat_id_);
    media_cache_.resolve(cached); clear_cached_file_ids(cached);
    const char* key; json_t* message;
    if (json_is_object(cached)) json_object_foreach(cached, key, message) { if (matchesTopic(message)) history_items_.push_back(parseMessage(message)); }
    if (cached) json_decref(cached);
    history_cached_ = !history_items_.empty();
    rebuildHistory();
    history_cursor_ = 0; history_target_ = 60;
    history_pending_ = history_end_ = false; history_done_ = !ready_;
    history_error_.clear();
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
    if (current_topic_) {
        json_object_set_new(req, "@type", json_string("getForumTopicHistory"));
        json_object_set_new(req, "forum_topic_id", json_integer(current_topic_));
        json_object_del(req, "only_local");
    }
    json_object_set_new(req, "@extra", extra);
    sendJson(req);
    status_ = "Loading messages...";
}

void TelegramClient::openStickers() {
    ++sticker_session_;
    sticker_packs_ = {{0, "Недавние"}, {1, "Избранные"}};
    sticker_set_ids_ = {0, 0};
    selectStickerPack(0);
    if (!ready_ || !current_chat_id_) return;
    json_t* root = json_pack("{s:s,s:{s:s}}", "@type", "getInstalledStickerSets", "sticker_type", "@type", "stickerTypeRegular");
    json_object_set_new(root, "@extra", json_pack("{s:s,s:I,s:i}", "kind", "sticker_packs", "chat", (json_int_t)current_chat_id_, "session", (int)sticker_session_));
    sendJson(root);
}

void TelegramClient::selectStickerPack(int index) {
    if (index < 0 || index >= (int)sticker_set_ids_.size()) return;
    ++sticker_generation_; stickers_.clear();
    sticker_status_ = ready_ ? "Загрузка стикеров..." : "Нет подключения к Telegram";
    if (!ready_ || !current_chat_id_) return;
    json_t* root = json_pack("{s:s}", "@type", index == 0 ? "getRecentStickers" : index == 1 ? "getFavoriteStickers" : "getStickerSet");
    if (!index) json_object_set_new(root, "is_attached", json_false());
    if (index >= 2) json_object_set_new(root, "set_id", json_integer(sticker_set_ids_[index]));
    json_object_set_new(root, "@extra", json_pack("{s:s,s:I,s:i,s:i}", "kind", "sticker_items", "chat", (json_int_t)current_chat_id_, "session", (int)sticker_session_, "generation", (int)sticker_generation_));
    sendJson(root);
}

void TelegramClient::ensureSticker(size_t index) {
    if (index < stickers_.size() && stickers_[index].path.empty()) requestFile(stickers_[index].preview, 32);
}

bool TelegramClient::sendSticker(size_t index) {
    if (!ready_ || !current_chat_id_ || topics_open_ || index >= stickers_.size()) return false;
    const auto& s = stickers_[index];
    json_t* input = json_pack("{s:s,s:{s:s,s:i},s:i,s:i}", "@type", "inputSticker", "sticker", "@type", "inputFileId", "id", s.file, "width", s.width, "height", s.height);
    json_t* content = json_pack("{s:s,s:o,s:s}", "@type", "inputMessageSticker", "sticker", input, "emoji", s.emoji.c_str());
    json_t* root = json_pack("{s:s,s:I,s:o,s:s}", "@type", "sendMessage", "chat_id", (json_int_t)current_chat_id_, "input_message_content", content, "@extra", "send_message");
    sendJson(root); status_ = "Sending sticker...";
    return true;
}

void TelegramClient::requestCompose() {
    if (current_chat_id_ && ready_ && !topics_open_) setInput(InputKind::Message, "Compose message");
}

void TelegramClient::loadReactions(int64_t id) {
    reaction_message_ = id; reaction_choices_.clear(); reaction_pending_ = false;
    reaction_status_ = ready_ ? "Загрузка реакций..." : "Нет подключения";
    if (!ready_ || !current_chat_id_ || id <= 0) return;
    json_t* request = json_pack("{s:s,s:I,s:I,s:i}", "@type", "getMessageAvailableReactions",
        "chat_id", (json_int_t)current_chat_id_, "message_id", (json_int_t)id, "row_size", 8);
    json_object_set_new(request, "@extra", json_pack("{s:s,s:I,s:I}", "kind", "reaction_choices", "chat", (json_int_t)current_chat_id_, "message", (json_int_t)id));
    sendJson(request);
}
void TelegramClient::toggleReaction(int64_t id, const std::string& emoji) {
    if (!ready_ || id != reaction_message_ || reaction_pending_) return;
    bool chosen = false;
    for (const auto& message : messages_) if (message.id == id)
        for (const auto& reaction : message.reactions) if (reaction.emoji == emoji) chosen = reaction.chosen;
    json_t* request = json_pack("{s:s,s:I,s:I,s:{s:s,s:s}}", "@type", chosen ? "removeMessageReaction" : "addMessageReaction",
        "chat_id", (json_int_t)current_chat_id_, "message_id", (json_int_t)id, "reaction_type", "@type", "reactionTypeEmoji", "emoji", emoji.c_str());
    if (!chosen) { json_object_set_new(request, "is_big", json_false()); json_object_set_new(request, "update_recent_reactions", json_true()); }
    json_object_set_new(request, "@extra", json_pack("{s:s,s:I,s:I}", "kind", "reaction_send", "chat", (json_int_t)current_chat_id_, "message", (json_int_t)id));
    reaction_pending_ = true; reaction_status_ = "Отправка..."; sendJson(request);
}

void TelegramClient::leaveChat() {
    current_topic_ = 0; topics_open_ = topics_pending_ = false; topics_.clear(); ++topics_generation_;
    reaction_message_ = 0; reaction_choices_.clear(); reaction_pending_ = false;
    ++history_generation_;
    history_pending_ = false;
    history_done_ = true;
    history_items_.clear();
    current_chat_id_ = 0;
    current_chat_title_.clear();
    messages_.clear();
    input_kind_ = InputKind::None;
    status_ = "Online";
}

void TelegramClient::close() {
    if (worker_started_ && !closed_) sendRaw("{\"@type\":\"close\"}");
}

std::string TelegramClient::currentChatAvatar() const {
    for (const auto& chat : known_chats_) if (chat.id == current_chat_id_) return chat.avatar_path;
    return "";
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
