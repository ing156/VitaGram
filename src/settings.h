#pragma once
#include <jansson.h>
#include <cstdio>
#include <algorithm>
#include <string>

struct AppSettings {
    int retention = 2, capacity = 2, theme = 0, font = 1, request_batch_size = 32; // Оптимизация: увеличил с 16 до 32 для более быстрой загрузки
    bool show_threads = false;
    bool proxy_enabled = false;
    std::string proxy_server, proxy_secret;
    int proxy_port = 443;
    int cache_storage = 0;
    std::string download_dir = "ux0:downloads/vitagram";
    static int days(int i) { const int values[] = {1, 7, 30, 90, 0}; return values[i]; }
    static int megabytes(int i) { const int values[] = {128, 256, 512, 1024, 2048, 0}; return values[i]; }
    static int requestBatchSize(int i) { const int values[] = {4, 8, 16, 32, 64}; return values[std::max(0, std::min(4, i))]; } // Оптимизация: сдвинул значения вправо (прежн. индекс 1→теперь значение 32)
    static int requestBatchIndex(int value) {
        const int values[] = {4, 8, 16, 32, 64};
        int best = 0;
        for (int i = 1; i < 5; ++i) if (std::abs(values[i] - value) < std::abs(values[best] - value)) best = i;
        return best;
    }
    float fontScale() const { return 0.9f + font * 0.1f; }
    void load() {
        json_error_t error{};
        json_t* root = json_load_file("ux0:data/vitagram/settings.json", 0, &error);
        if (!json_is_object(root)) { if (root) json_decref(root); return; }
        auto value = [&](const char* key, int fallback, int maximum) {
            json_t* v = json_object_get(root, key);
            return json_is_integer(v) ? std::max(0, std::min(maximum, (int)json_integer_value(v))) : fallback;
        };
        retention = value("retention", 2, 4); capacity = value("capacity", 2, 5);
        theme = value("theme", 0, 1); font = value("font", 1, 2);
        request_batch_size = value("request_batch_size", 16, 64);
        cache_storage = value("cache_storage", 0, 1);
        show_threads = json_is_true(json_object_get(root, "show_threads"));
        proxy_enabled = json_is_true(json_object_get(root, "proxy_enabled"));
        proxy_port = value("proxy_port", 443, 65535);
        auto string_value = [&](const char* key) {
            const char* v = json_string_value(json_object_get(root, key));
            return std::string(v ? v : "");
        };
        proxy_server = string_value("proxy_server");
        proxy_secret = string_value("proxy_secret");
        std::string saved_download_dir = string_value("download_dir");
        if (!saved_download_dir.empty() && saved_download_dir != "ux0:download/vitagram" &&
            saved_download_dir != "ux0:/download/vitagram") download_dir = saved_download_dir;
        json_decref(root);
    }
    bool save() const {
        json_t* root = json_pack("{s:i,s:i,s:i,s:i,s:i,s:b}", "retention", retention, "capacity", capacity,
            "theme", theme, "font", font, "request_batch_size", request_batch_size, "show_threads", show_threads);
        json_object_set_new(root, "proxy_enabled", json_boolean(proxy_enabled));
        json_object_set_new(root, "proxy_server", json_string(proxy_server.c_str()));
        json_object_set_new(root, "proxy_port", json_integer(proxy_port));
        json_object_set_new(root, "proxy_secret", json_string(proxy_secret.c_str()));
        json_object_set_new(root, "cache_storage", json_integer(cache_storage));
        json_object_set_new(root, "download_dir", json_string(download_dir.c_str()));
        bool ok = json_dump_file(root, "ux0:data/vitagram/settings.json.tmp", JSON_COMPACT) == 0;
        json_decref(root);
        return ok && std::rename("ux0:data/vitagram/settings.json.tmp", "ux0:data/vitagram/settings.json") == 0;
    }
};
