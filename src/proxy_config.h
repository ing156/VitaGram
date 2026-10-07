#pragma once
#include <string>

enum class ProxyKind { MTProto = 0, Socks5 = 1, Https = 2 };

// Connection proxy as handed to TDLib. `secret` is used by MTProto only;
// `username`/`password` by SOCKS5 and HTTPS (HTTP CONNECT).
struct ProxyConfig {
    ProxyKind kind = ProxyKind::MTProto;
    std::string server, secret, username, password;
    int port = 443;

    bool complete() const {
        return !server.empty() && port >= 1 && port <= 65535 && (kind != ProxyKind::MTProto || !secret.empty());
    }
};

inline ProxyKind proxy_kind_from_int(int value) {
    return value == 1 ? ProxyKind::Socks5 : value == 2 ? ProxyKind::Https : ProxyKind::MTProto;
}
