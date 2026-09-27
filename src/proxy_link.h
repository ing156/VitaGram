#pragma once
#include <string>

struct ProxyLink {
    std::string server, secret;
    int port = 0;
};

enum class ProxyLinkResult { NotProxy, Invalid, Valid };
// Does not change out unless the complete link is valid. Never echoes secret in errors.
ProxyLinkResult parse_proxy_link(const std::string& url, ProxyLink& out, std::string& error);
bool is_proxy_link(const std::string& url);
