#pragma once
#include "proxy_config.h"
#include <string>

struct ProxyLink {
    std::string server, secret, username, password;
    int port = 0;
    ProxyKind kind = ProxyKind::MTProto; // tg://socks links give Socks5
};

enum class ProxyLinkResult { NotProxy, Invalid, Valid };
// Does not change out unless the complete link is valid. Never echoes secret in errors.
ProxyLinkResult parse_proxy_link(const std::string& url, ProxyLink& out, std::string& error);
bool is_proxy_link(const std::string& url);
