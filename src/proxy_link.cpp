#include "proxy_link.h"
#include <algorithm>
#include <cctype>

namespace {
int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool unescape(const std::string& src, std::string& dst) {
    dst.clear();
    for (size_t i = 0; i < src.size(); ++i) {
        unsigned char c = src[i];
        if (c == '%') {
            if (i + 2 >= src.size() || hex(src[i+1]) < 0 || hex(src[i+2]) < 0) return false;
            c = hex(src[i+1]) * 16 + hex(src[i+2]); i += 2;
        }
        // Preserve '+' for base64 secrets. Control characters are never valid.
        if (c <= 32 || c >= 127) return false;
        dst += c;
    }
    return true;
}
}

bool is_proxy_link(const std::string& url) {
    std::string base = url.substr(0, url.find_first_of("?#"));
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return std::tolower(c); });
    if (!base.empty() && base.back() == '/') base.pop_back();
    if (base == "tg://proxy" || base == "tg:proxy") return true;
    for (const char* host : {"t.me", "telegram.me", "telegram.dog"})
        for (const char* scheme : {"https://", "http://", ""})
            if (base == std::string(scheme) + host + "/proxy") return true;
    return false;
}

ProxyLinkResult parse_proxy_link(const std::string& url, ProxyLink& out, std::string& error) {
    error.clear();
    if (!is_proxy_link(url)) return ProxyLinkResult::NotProxy;
    auto invalid = [&](const char* message) { error = message; return ProxyLinkResult::Invalid; };
    if (url.size() > 4096) return invalid("Слишком длинная ссылка MTProxy");
    size_t query = url.find('?'), fragment = url.find('#');
    if (query == std::string::npos || (fragment != std::string::npos && query > fragment))
        return invalid("В ссылке нет параметров MTProxy");
    std::string args = url.substr(query + 1, fragment == std::string::npos ? fragment : fragment - query - 1);
    ProxyLink candidate;
    bool server_seen = false, port_seen = false, secret_seen = false;
    for (size_t pos = 0; pos < args.size();) {
        size_t end = args.find('&', pos);
        if (end == std::string::npos) end = args.size();
        std::string pair = args.substr(pos, end - pos);
        size_t eq = pair.find('=');
        std::string key, value;
        if (!unescape(pair.substr(0, eq), key) ||
            !unescape(eq == std::string::npos ? "" : pair.substr(eq + 1), value))
            return invalid("Некорректное кодирование ссылки MTProxy");
        if (key == "server") {
            if (server_seen) return invalid("Параметр server указан дважды");
            server_seen = true; candidate.server = value;
        } else if (key == "secret") {
            if (secret_seen) return invalid("Параметр secret указан дважды");
            secret_seen = true; candidate.secret = value;
        } else if (key == "port") {
            if (port_seen) return invalid("Параметр port указан дважды");
            port_seen = true;
            if (value.empty() || value.size() > 5) return invalid("Порт должен быть от 1 до 65535");
            for (char c : value) {
                if (c < '0' || c > '9') return invalid("Порт должен быть от 1 до 65535");
                candidate.port = candidate.port * 10 + c - '0';
            }
        }
        pos = end + 1;
    }
    if (!server_seen || !port_seen || !secret_seen || candidate.server.empty() || candidate.secret.empty())
        return invalid("В ссылке нужны server, port и secret");
    if (candidate.port < 1 || candidate.port > 65535) return invalid("Порт должен быть от 1 до 65535");
    if (candidate.server.size() > 253 || candidate.server.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:[]") != std::string::npos)
        return invalid("Некорректный адрес сервера MTProxy");
    // TDLib validates the decoded secret (hex, base64 or base64url) when applying.
    if (candidate.secret.size() > 1023 || candidate.secret.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_/+=") != std::string::npos)
        return invalid("Некорректный secret MTProxy");
    out = std::move(candidate);
    return ProxyLinkResult::Valid;
}
