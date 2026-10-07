#include "proxy_link.h"
#include "i18n.h"
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

namespace {
// 'proxy' for MTProxy links, 'socks' for SOCKS5; empty when url is neither.
std::string link_path(const std::string& url) {
    std::string base = url.substr(0, url.find_first_of("?#"));
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return std::tolower(c); });
    if (!base.empty() && base.back() == '/') base.pop_back();
    for (const char* path : {"proxy", "socks"}) {
        if (base == std::string("tg://") + path || base == std::string("tg:") + path) return path;
        for (const char* host : {"t.me", "telegram.me", "telegram.dog"})
            for (const char* scheme : {"https://", "http://", ""})
                if (base == std::string(scheme) + host + "/" + path) return path;
    }
    return "";
}
}

bool is_proxy_link(const std::string& url) { return !link_path(url).empty(); }

ProxyLinkResult parse_proxy_link(const std::string& url, ProxyLink& out, std::string& error) {
    error.clear();
    if (!is_proxy_link(url)) return ProxyLinkResult::NotProxy;
    auto invalid = [&](const std::string& message) { error = message; return ProxyLinkResult::Invalid; };
    if (url.size() > 4096) return invalid(tr("proxy.err.too_long"));
    size_t query = url.find('?'), fragment = url.find('#');
    if (query == std::string::npos || (fragment != std::string::npos && query > fragment))
        return invalid(tr("proxy.err.no_params"));
    std::string args = url.substr(query + 1, fragment == std::string::npos ? fragment : fragment - query - 1);
    ProxyLink candidate;
    const bool socks = link_path(url) == "socks";
    if (socks) candidate.kind = ProxyKind::Socks5;
    bool server_seen = false, port_seen = false, secret_seen = false, user_seen = false, pass_seen = false;
    for (size_t pos = 0; pos < args.size();) {
        size_t end = args.find('&', pos);
        if (end == std::string::npos) end = args.size();
        std::string pair = args.substr(pos, end - pos);
        size_t eq = pair.find('=');
        std::string key, value;
        if (!unescape(pair.substr(0, eq), key) ||
            !unescape(eq == std::string::npos ? "" : pair.substr(eq + 1), value))
            return invalid(tr("proxy.err.bad_encoding"));
        if (key == "server") {
            if (server_seen) return invalid(tr("proxy.err.server_twice"));
            server_seen = true; candidate.server = value;
        } else if (socks && (key == "user" || key == "pass")) {
            bool& seen = key == "user" ? user_seen : pass_seen;
            if (seen) return invalid(tr("proxy.err.credentials_twice"));
            seen = true;
            if (value.size() > 255) return invalid(tr("proxy.err.bad_credentials"));
            (key == "user" ? candidate.username : candidate.password) = value;
        } else if (!socks && key == "secret") {
            if (secret_seen) return invalid(tr("proxy.err.secret_twice"));
            secret_seen = true; candidate.secret = value;
        } else if (key == "port") {
            if (port_seen) return invalid(tr("proxy.err.port_twice"));
            port_seen = true;
            if (value.empty() || value.size() > 5) return invalid(tr("proxy.err.port_range"));
            for (char c : value) {
                if (c < '0' || c > '9') return invalid(tr("proxy.err.port_range"));
                candidate.port = candidate.port * 10 + c - '0';
            }
        }
        pos = end + 1;
    }
    if (!server_seen || !port_seen || (!socks && !secret_seen) || candidate.server.empty() ||
        (!socks && candidate.secret.empty()))
        return invalid(tr("proxy.err.missing_fields"));
    if (candidate.port < 1 || candidate.port > 65535) return invalid(tr("proxy.err.port_range"));
    if (candidate.server.size() > 253 || candidate.server.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:[]") != std::string::npos)
        return invalid(tr("proxy.err.bad_server"));
    // TDLib validates the decoded secret (hex, base64 or base64url) when applying.
    if (candidate.secret.size() > 1023 || candidate.secret.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_/+=") != std::string::npos)
        return invalid(tr("proxy.err.bad_secret"));
    out = std::move(candidate);
    return ProxyLinkResult::Valid;
}
