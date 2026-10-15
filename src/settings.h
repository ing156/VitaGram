#pragma once
#include "diag_log.h"
#include "scroll_repeat.h"
#include "proxy_config.h"
#include "secure_store.h"
#include "vault.h"
#include <jansson.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>

struct AppSettings {
    int retention = 2, capacity = 2, theme = 1, font = 5, request_batch_size = 64;
    int opacity = 75;       // percent of the surface PNGs (rows, message cards) over the wallpaper, 5% steps
    int avatar_shape = 0;   // 0 rounded square, 1 circle
    int scroll_speed = 0;   // Normal, Fast, Very fast: 2, 4, 6 pages per second.
    bool show_threads = false;
    bool gyroscope_enabled = true;
    bool show_archive = true;  // Archive tab in the chat list folders
    unsigned log_mask = 0;     // diag_log::Kind bits; every diagnostic log is off by default
    static constexpr int kSpoilerResetChoices = 4;
    int spoiler_reset = 3; // 0 off, 1 chat exit, 2 app leave, 3 both.
    bool spoilerHideChat() const { return (spoiler_reset & 1) != 0; }
    bool spoilerHideApp() const { return (spoiler_reset & 2) != 0; }
    bool recording_shortcuts = true; // L/R open the video/voice recording dialogs from chat history.
    bool check_updates = true; // look for a newer GitHub release shortly after startup
    // With the vault on: ask for the key again after this much inactivity (index into lockMinutes),
    // and after sleep or leaving the app with the PS button.
    int auto_lock = 3;
    bool lock_on_leave = true;
    bool proxy_enabled = false;
    std::string proxy_server, proxy_secret;
    int proxy_port = 443;
    int proxy_type = 0; // ProxyKind: 0 MTProto, 1 SOCKS5, 2 HTTPS
    std::string proxy_username, proxy_password;
    ProxyConfig proxyConfig() const {
        ProxyConfig c;
        c.kind = proxy_kind_from_int(proxy_type); c.server = proxy_server; c.port = proxy_port;
        c.secret = proxy_secret; c.username = proxy_username; c.password = proxy_password;
        return c;
    }
    int cache_storage = 0;
    std::string download_dir = "ux0:downloads/vitagram";
    std::string language; // "" = follow the console language, otherwise a code from app0:lang/*.lang
    // load() found a settings.json not sealed by the vault although the vault is on: it was replaced
    // outside VitaGram (or never re-saved after switching encryption on) and has been ignored.
    bool rejected = false;
    // load() read a settings.json (sealed with the vault key when the vault is on).
    bool loaded = false;
    static int days(int i) { const int values[] = {1, 7, 30, 90, 0}; return values[i]; }
    static int megabytes(int i) { const int values[] = {128, 256, 512, 1024, 2048, 0}; return values[i]; }
    static int requestBatchSize(int i) { const int values[] = {4, 8, 16, 32, 64}; return values[std::max(0, std::min(4, i))]; } // Оптимизация: сдвинул значения вправо (прежн. индекс 1→теперь значение 32)
    static int requestBatchIndex(int value) {
        const int values[] = {4, 8, 16, 32, 64};
        int best = 0;
        for (int i = 1; i < 5; ++i) if (std::abs(values[i] - value) < std::abs(values[best] - value)) best = i;
        return best;
    }
    static constexpr int kLockChoices = 7;
    static int lockMinutes(int i) { const int values[] = {0, 1, 2, 5, 10, 15, 30}; return values[std::max(0, std::min(kLockChoices - 1, i))]; }
    static constexpr int kMinOpacity = 20, kMaxOpacity = 100, kOpacityStep = 5;
    // The theme alone is also kept in a plain file: with the vault on, settings.json is sealed, yet the
    // unlock screens (shown before the key is entered) must already use the chosen theme.
    static constexpr int kThemeChoices = 3;
    bool isDark() const { return theme != 0; }
    static constexpr const char* kThemeHint = "ux0:data/vitagram/theme.txt";
    static int themeHint(int fallback) {
        int value = fallback;
        if (FILE* file = std::fopen(kThemeHint, "r")) {
            char c = 0;
            if (std::fread(&c, 1, 1, file) == 1 && c >= '0' && c < '0' + kThemeChoices) value = c - '0';
            std::fclose(file);
        }
        return value;
    }
    void saveThemeHint() const {
        if (themeHint(-1) == theme) return;
        if (FILE* file = std::fopen(kThemeHint, "w")) { std::fputc('0' + theme, file); std::fclose(file); }
    }
    float fontScale() const { return 0.7f + font * 0.1f; }
    void load() {
        // Sealed when the vault is on; the vault must be unlocked first, or this reads defaults.
        // Always starts from the defaults, so nothing read earlier survives a refused file.
        static constexpr const char* path = "ux0:data/vitagram/settings.json";
        *this = AppSettings();
        const bool strict = vault::settings_must_be_sealed();
        std::string text;
        if (!vault::read_file(path, 1 << 20, text, nullptr, nullptr, strict ? vault::Require::Vault : vault::Require::Any)) {
            struct stat info{};
            rejected = strict && stat(path, &info) == 0 && !vault::is_vault_sealed(path);
            return;
        }
        json_error_t error{};
        json_t* root = json_loadb(text.data(), text.size(), 0, &error);
        if (!json_is_object(root)) { if (root) json_decref(root); return; }
        loaded = true;
        auto value = [&](const char* key, int fallback, int maximum) {
            json_t* v = json_object_get(root, key);
            return json_is_integer(v) ? std::max(0, std::min(maximum, (int)json_integer_value(v))) : fallback;
        };
        retention = value("retention", 2, 4); capacity = value("capacity", 2, 5);
        theme = value("theme", 1, kThemeChoices - 1); font = value("font_size", 5, 6);
        request_batch_size = value("request_batch_size", 64, 64);
        opacity = std::max(kMinOpacity, std::min(kMaxOpacity, (value("opacity", 75, 100) + kOpacityStep / 2) / kOpacityStep * kOpacityStep));
        avatar_shape = value("avatar_shape", 0, 1);
        if (json_t* v = json_object_get(root, "scroll_speed"))
            if (json_is_integer(v) && json_integer_value(v) >= 0 && json_integer_value(v) < ScrollRepeat::kSpeedChoices)
                scroll_speed = (int)json_integer_value(v);
        cache_storage = value("cache_storage", 0, 1);
        auto_lock = value("auto_lock", 3, kLockChoices - 1);
        if (json_t* v = json_object_get(root, "lock_on_leave"))
            if (json_is_boolean(v)) lock_on_leave = json_is_true(v);
        if (json_t* v = json_object_get(root, "spoiler_reset")) {
            if (json_is_integer(v) && json_integer_value(v) >= 0 && json_integer_value(v) < kSpoilerResetChoices)
                spoiler_reset = (int)json_integer_value(v);
        } else {
            // Preserve choices made by the earlier two-switch UI, then save only the new mode.
            bool chat = true, app = true;
            if (json_t* v = json_object_get(root, "spoiler_hide_chat"))
                if (json_is_boolean(v)) chat = json_is_true(v);
            if (json_t* v = json_object_get(root, "spoiler_hide_app"))
                if (json_is_boolean(v)) app = json_is_true(v);
            spoiler_reset = (chat ? 1 : 0) | (app ? 2 : 0);
        }
        if (json_t* v = json_object_get(root, "recording_shortcuts"))
            if (json_is_boolean(v)) recording_shortcuts = json_is_true(v);
        show_threads = json_is_true(json_object_get(root, "show_threads"));
        if (json_t* v = json_object_get(root, "check_updates")) check_updates = json_is_true(v);
        if (json_t* v = json_object_get(root, "gyroscope_enabled"))
            if (json_is_boolean(v)) gyroscope_enabled = json_is_true(v);
        if (json_t* v = json_object_get(root, "show_archive"))
            if (json_is_boolean(v)) show_archive = json_is_true(v);
        if (json_t* v = json_object_get(root, "log_mask"))
            if (json_is_integer(v)) log_mask = (unsigned)json_integer_value(v) & diag_log::kAll;
        proxy_enabled = json_is_true(json_object_get(root, "proxy_enabled"));
        proxy_port = value("proxy_port", 443, 65535);
        proxy_type = value("proxy_type", 0, 2);
        auto string_value = [&](const char* key) {
            const char* v = json_string_value(json_object_get(root, key));
            return std::string(v ? v : "");
        };
        proxy_server = string_value("proxy_server");
        // Secrets are encrypted at rest; plain-text values from older versions are re-saved encrypted.
        bool migrate = false;
        auto secret_value = [&](const char* key) {
            std::string stored = string_value(key), plain;
            secure_store::reveal(stored, plain); // Undecryptable value (other console): forget it.
            if (!stored.empty() && !secure_store::is_protected(stored) && secure_store::available()) migrate = true;
            return plain;
        };
        proxy_secret = secret_value("proxy_secret");
        proxy_username = secret_value("proxy_username");
        proxy_password = secret_value("proxy_password");
        language = string_value("language");
        std::string saved_download_dir = string_value("download_dir");
        if (!saved_download_dir.empty() && saved_download_dir != "ux0:download/vitagram" &&
            saved_download_dir != "ux0:/download/vitagram") download_dir = saved_download_dir;
        json_decref(root);
        saveThemeHint();
        if (migrate) save();
    }
    bool save() const {
        json_t* root = json_pack("{s:i,s:i,s:i,s:i,s:i,s:b}", "retention", retention, "capacity", capacity,
            "theme", theme, "font_size", font, "request_batch_size", request_batch_size, "show_threads", show_threads);
        json_object_set_new(root, "gyroscope_enabled", json_boolean(gyroscope_enabled));
        json_object_set_new(root, "show_archive", json_boolean(show_archive));
        json_object_set_new(root, "opacity", json_integer(opacity));
        json_object_set_new(root, "avatar_shape", json_integer(avatar_shape));
        json_object_set_new(root, "scroll_speed", json_integer(scroll_speed));
        json_object_set_new(root, "log_mask", json_integer(log_mask));
        json_object_set_new(root, "spoiler_reset", json_integer(spoiler_reset));
        json_object_set_new(root, "recording_shortcuts", json_boolean(recording_shortcuts));
        json_object_set_new(root, "check_updates", json_boolean(check_updates));
        json_object_set_new(root, "auto_lock", json_integer(auto_lock));
        json_object_set_new(root, "lock_on_leave", json_boolean(lock_on_leave));
        json_object_set_new(root, "proxy_enabled", json_boolean(proxy_enabled));
        json_object_set_new(root, "proxy_server", json_string(proxy_server.c_str()));
        json_object_set_new(root, "proxy_port", json_integer(proxy_port));
        json_object_set_new(root, "proxy_secret", json_string(secure_store::protect(proxy_secret).c_str()));
        json_object_set_new(root, "proxy_type", json_integer(proxy_type));
        json_object_set_new(root, "proxy_username", json_string(secure_store::protect(proxy_username).c_str()));
        json_object_set_new(root, "proxy_password", json_string(secure_store::protect(proxy_password).c_str()));
        json_object_set_new(root, "cache_storage", json_integer(cache_storage));
        json_object_set_new(root, "download_dir", json_string(download_dir.c_str()));
        json_object_set_new(root, "language", json_string(language.c_str()));
        saveThemeHint();
        char* dumped = json_dumps(root, JSON_COMPACT);
        json_decref(root);
        if (!dumped) return false;
        bool ok = vault::write_file_atomic("ux0:data/vitagram/settings.json", dumped);
        std::free(dumped);
        if (ok) vault::note_settings_saved(); // sealed now: from here on a missing file is noticed
        return ok;
    }
};

// After load() with the vault on: the proxy policy cannot be confirmed - settings.json was refused,
// or it should exist (saved under this vault) but could not be read, or the vault state itself is
// unsigned. The network must then wait for the user to save the proxy settings (F14).
inline bool proxy_policy_unknown(const AppSettings& settings) {
    return vault::settings_must_be_sealed() && !settings.loaded &&
           (settings.rejected || vault::settings_expected() || vault::state_unauthenticated());
}

