#pragma once
#include <memory>
#include <string>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <cmath>
#include "proxy_config.h"

struct vita2d_texture;

inline std::pair<int, int> webapp_page_point(int rotation, int x, int y, int width = 960, int height = 392) {
    x = std::clamp(x, 0, width - 1); y = std::clamp(y, 0, height - 1);
    if (rotation == 1) return {y, width - 1 - x};
    if (rotation == 3) return {height - 1 - y, x};
    return {x, y};
}

inline std::pair<int, int> webapp_page_delta(int rotation, int dx, int dy) {
    if (rotation == 1) return {dy, -dx};
    if (rotation == 3) return {-dy, dx};
    return {dx, dy};
}

struct WebAppGesture {
    struct Action { bool click = false; int dx = 0, dy = 0; };
    bool held = false, captured = false, dragged = false;
    int id = -1, start_x = 0, start_y = 0, last_x = 0, last_y = 0;
    int64_t since = 0;
    float vx = 0, vy = 0, remainder_x = 0, remainder_y = 0;
    void reset() { *this = WebAppGesture{}; }
    Action update(bool enabled, int fingers, int finger_id, int x, int y, int width, int height, int64_t now) {
        Action action;
        if (!enabled) { reset(); held = fingers > 0; return action; }
        const float elapsed = since && now > since ? (now - since) / 1000000.f : 0;
        const float dt = std::min(0.05f, elapsed);
        since = now;
        if (fingers) {
            if (!held) {
                held = true; id = finger_id; start_x = last_x = x; start_y = last_y = y;
                captured = fingers == 1 && x >= 0 && y >= 0 && x < width && y < height;
                dragged = false; vx = vy = remainder_x = remainder_y = 0;
                return action;
            }
            if (fingers != 1 || finger_id != id) captured = false;
            if (!captured) return action;
            const int dx = x - start_x, dy = y - start_y;
            if (!dragged && (std::abs(dx) > 10 || std::abs(dy) > 10)) dragged = true;
            if (dragged) {
                action.dx = last_x - x; action.dy = last_y - y;
                if (elapsed > 0) {
                    vx = std::clamp(action.dx / elapsed, -2500.f, 2500.f);
                    vy = std::clamp(action.dy / elapsed, -2500.f, 2500.f);
                }
                last_x = x; last_y = y;
            }
            return action;
        }
        if (held) {
            action.click = captured && !dragged;
            if (!captured || !dragged) vx = vy = 0;
            held = captured = false;
            return action;
        }
        remainder_x += vx * dt; remainder_y += vy * dt;
        action.dx = int(remainder_x); action.dy = int(remainder_y);
        remainder_x -= action.dx; remainder_y -= action.dy;
        const float decay = std::exp(-7.f * dt);
        vx *= decay; vy *= decay;
        if (std::abs(vx) < 5) vx = 0;
        if (std::abs(vy) < 5) vy = 0;
        return action;
    }
};

inline bool webapp_proxy_url(bool enabled, const ProxyConfig& proxy, std::string& url) {
    url.clear();
    // MTProxy carries Telegram traffic only; Mini Apps use a direct connection.
    if (!enabled || proxy.kind == ProxyKind::MTProto) return true;
    if (!proxy.complete() || (proxy.kind != ProxyKind::Socks5 && proxy.kind != ProxyKind::Https) ||
        proxy.username.size() > 255 || proxy.password.size() > 255 ||
        proxy.username.find('\0') != std::string::npos || proxy.password.find('\0') != std::string::npos ||
        proxy.server.size() > 253 || proxy.server.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos)
        return false;
    const auto encode = [](const std::string& value) {
        std::string result;
        constexpr char hex[] = "0123456789ABCDEF";
        for (unsigned char c : value) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '.' || c == '_' || c == '~') result += c;
            else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
        }
        return result;
    };
    url = proxy.kind == ProxyKind::Socks5 ? "socks5h://" : "http://";
    if (!proxy.username.empty() || !proxy.password.empty())
        url += encode(proxy.username) + ":" + encode(proxy.password) + "@";
    url += proxy.server + ":" + std::to_string(proxy.port);
    return true;
}

inline bool webapp_passive_event(const std::string& type) {
    // This top-level view has neither an iframe parent nor a swipe-to-dismiss gesture.
    return type == "iframe_ready" || type == "web_app_setup_swipe_behavior" ||
        type == "web_app_trigger_haptic_feedback"; // Vita has no vibration motor.
}

inline void webapp_move_pointer(float& x, float& y, float axis_x, float axis_y, int64_t elapsed_us, int height = 392) {
    const float dt = std::clamp<int64_t>(elapsed_us, 0, 50000) / 1000000.f;
    x = std::clamp(x + axis_x * 600.f * dt, 0.f, 959.f);
    y = std::clamp(y + axis_y * 600.f * dt, 0.f, float(height - 1));
}

inline bool webapp_color(const std::string& value, unsigned& color) {
    if (value.size() != 7 || value[0] != '#') return false;
    unsigned rgb = 0;
    for (int i = 1; i < 7; ++i) {
        const auto c = value[i];
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return false;
        rgb = rgb * 16 + digit;
    }
    color = 0xff000000u | ((rgb >> 16) & 255) | (rgb & 0xff00) | ((rgb & 255) << 16);
    return true;
}

// HTTPS origins are compared before delivering any native bridge event.
inline std::string webapp_origin(const std::string& url) {
    if (url.compare(0, 8, "https://")) return {};
    auto authority = url.substr(8, url.find_first_of("/?#", 8) - 8);
    if (authority.empty() || authority.find_first_of("@\\ \t\r\n") != std::string::npos) return {};
    for (auto& c : authority) {
        if ((unsigned char)c < 33 || (unsigned char)c > 126) return {};
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    if (authority.size() > 4 && authority.compare(authority.size() - 4, 4, ":443") == 0) authority.resize(authority.size() - 4);
    return "https://" + authority;
}

class WebAppView {
public:
    WebAppView();
    ~WebAppView();
    WebAppView(const WebAppView&) = delete;
    WebAppView& operator=(const WebAppView&) = delete;
    bool open(const std::string& url, bool same_origin, bool proxy_enabled, const ProxyConfig& proxy);
    void close();
    void shutdown();
    bool active() const;
    void pump();
    void move(int x, int y);
    void click(int x, int y, bool down);
    void scroll(int delta, int horizontal = 0);
    void orientation(int rotation, bool fullscreen = false);
    int rotation() const;
    bool fullscreen() const;
    void fontScale(float scale);
    void evaluate(const std::string& script);
    void event(const std::string& type, const std::string& json = "{}");
    std::string takeEvents();
    std::string takeLink();
    const std::string& error() const;
    bool loading() const;
    vita2d_texture* texture() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
