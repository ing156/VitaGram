#include "webapp_view.h"
#include "webapp_theme.h"
#include "webapp_diagnostics.h"
#include "webapp_invoice.h"
#include <cassert>
#include <regex>
#include <map>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cmath>

static std::map<std::string, unsigned> palette;
namespace ui_skin {
bool is_dark(bool fallback) { return fallback; }
unsigned color(const char* key, unsigned fallback) { auto it = palette.find(key); return it == palette.end() ? fallback : it->second; }
unsigned surface_color(unsigned fallback) { return fallback; }
}

int main() {
    auto* invoice = json_pack("{s:s}", "slug", "invoice_test-123=_");
    const auto cancelled = webapp_cancelled_invoice(invoice);
    auto* response = json_loads(cancelled.c_str(), 0, nullptr);
    assert(response && json_object_size(response) == 2);
    assert(std::string(json_string_value(json_object_get(response, "slug"))) == "invoice_test-123=_");
    assert(std::string(json_string_value(json_object_get(response, "status"))) == "cancelled");
    assert(json_string_length(json_object_get(invoice, "slug")) == 18);
    json_decref(response); json_decref(invoice);
    invoice = json_pack("{s:o}", "slug", json_stringn("bad\0name", 8));
    assert(webapp_cancelled_invoice(invoice).empty()); json_decref(invoice);
    const std::string oversized_slug(8193, 'x');
    invoice = json_pack("{s:s}", "slug", oversized_slug.c_str());
    assert(webapp_cancelled_invoice(invoice).empty()); json_decref(invoice);
    assert(webapp_cancelled_invoice(nullptr).empty());
    for (const char* raw : {"{}", "null", "[]", "{\"slug\":4}", "{\"slug\":\"\"}",
                            "{\"slug\":\"bad name\"}", "{\"slug\":\"bad\\\"name\"}", "{\"slug\":\"bad\\u0000name\"}"}) {
        auto* data = json_loads(raw, JSON_DECODE_ANY, nullptr);
        assert(webapp_cancelled_invoice(data).empty()); json_decref(data);
    }
    WebAppGesture gesture;
    int64_t gesture_time = 1000000;
    auto finger = [&](int fingers, int x, int y, bool enabled = true, int id = 7) {
        gesture_time += 16000;
        return gesture.update(enabled, fingers, id, x, y, 960, 544, gesture_time);
    };
    assert(!finger(1, 100, 100).click);
    assert(!finger(1, 105, 103).dy);
    assert(finger(0, 105, 103).click);
    assert(!finger(0, 105, 103).click);
    finger(1, 200, 300);
    auto drag = finger(1, 180, 250);
    assert(!drag.click && drag.dx == 20 && drag.dy == 50);
    assert(!finger(0, 180, 250).click);
    auto coast = finger(0, 180, 250);
    assert(coast.dx > 0 && coast.dy > 0);
    for (int i = 0; i < 200; ++i) finger(0, 180, 250);
    assert(gesture.vx == 0 && gesture.vy == 0);
    finger(1, 100, 500);
    assert(finger(0, 100, 500).click);
    finger(1, 100, -10);
    finger(1, 100, 100);
    assert(!finger(0, 100, 100).click);
    finger(1, 100, 100);
    finger(2, 120, 100);
    assert(!finger(0, 120, 100).click);
    finger(1, 100, 100);
    finger(1, 100, 100, true, 8);
    assert(!finger(0, 100, 100).click);
    finger(1, 100, 100, false);
    finger(1, 100, 100);
    assert(!finger(0, 100, 100).click);
    finger(1, 200, 300); finger(1, 200, 250);
    finger(0, 200, 250, false);
    assert(finger(0, 200, 250).dy == 0);
    for (int rotation : {0, 1, 3}) {
        const auto [dx, dy] = webapp_page_delta(rotation, 20, -30);
        const auto a = webapp_page_point(rotation, 400, 200, 960, 544);
        const auto b = webapp_page_point(rotation, 420, 170, 960, 544);
        assert(dx == b.first - a.first && dy == b.second - a.second);
    }
    ProxyConfig proxy;
    std::string proxy_url = "previous";
    assert(webapp_proxy_url(false, proxy, proxy_url) && proxy_url.empty());
    assert(webapp_proxy_url(true, proxy, proxy_url) && proxy_url.empty());
    proxy.server = "proxy.example"; proxy.secret = "secret";
    proxy_url = "http://previous-proxy:80";
    assert(webapp_proxy_url(true, proxy, proxy_url) && proxy_url.empty());
    proxy.kind = ProxyKind::Socks5; proxy.port = 1080;
    assert(webapp_proxy_url(true, proxy, proxy_url) && proxy_url == "socks5h://proxy.example:1080");
    proxy.username = "user@name"; proxy.password = "p:/%?#\\";
    assert(webapp_proxy_url(true, proxy, proxy_url) && proxy_url == "socks5h://user%40name:p%3A%2F%25%3F%23%5C@proxy.example:1080");
    proxy.kind = ProxyKind::Https; proxy.server = "127.0.0.1"; proxy.port = 8080;
    assert(webapp_proxy_url(true, proxy, proxy_url) && proxy_url == "http://user%40name:p%3A%2F%25%3F%23%5C@127.0.0.1:8080");
    for (const char* host : {"", "https://proxy", "proxy/evil", "proxy@evil", "proxy\\evil", "proxy\n", "proxy:80"}) {
        proxy.server = host;
        assert(!webapp_proxy_url(true, proxy, proxy_url) && proxy_url.empty());
    }
    proxy.server = "proxy.example";
    for (int port : {0, -1, 65536}) {
        proxy.port = port;
        assert(!webapp_proxy_url(true, proxy, proxy_url) && proxy_url.empty());
    }
    proxy.port = 65535;
    proxy.password = std::string("bad\0password", 12);
    assert(!webapp_proxy_url(true, proxy, proxy_url) && proxy_url.empty());
    proxy.password = std::string(256, 'a');
    assert(!webapp_proxy_url(true, proxy, proxy_url) && proxy_url.empty());
    assert(webapp_proxy_url(false, proxy, proxy_url) && proxy_url.empty());
    assert(webapp_passive_event("iframe_ready"));
    assert(webapp_passive_event("web_app_setup_swipe_behavior"));
    assert(webapp_passive_event("web_app_trigger_haptic_feedback"));
    for (const char* type : {"web_app_request_phone", "web_app_open_invoice", "web_app_request_write_access",
                             "web_app_invoke_custom_method", "unknown", "iframe_will_reload"})
        assert(!webapp_passive_event(type));
    float x = 480, y = 196;
    webapp_move_pointer(x, y, 0, 0, 16000);
    assert(x == 480 && y == 196);
    webapp_move_pointer(x, y, 1, -1, 10000000);
    assert(x == 510 && y == 166); // A stalled/resumed frame moves at most 50 ms.
    webapp_move_pointer(x, y, 1, 1, -1);
    assert(x == 510 && y == 166);
    x = 958; y = 390;
    webapp_move_pointer(x, y, 1, 1, 50000);
    assert(x == 959 && y == 391);
    x = y = 1;
    webapp_move_pointer(x, y, -1, -1, 50000);
    assert(x == 0 && y == 0);
    x = y = 10;
    for (int i = 0; i < 60; ++i) webapp_move_pointer(x, y, 0.1f, 0, 16667);
    assert(std::abs(x - 70) < 0.01f && y == 10);
    float other_x = 10, other_y = 10;
    for (int i = 0; i < 30; ++i) webapp_move_pointer(other_x, other_y, 0.1f, 0, 33333);
    assert(std::abs(x - other_x) < 0.01f && y == other_y);
    const char* log = "ux0:data/vitagram/webapp.log";
    std::filesystem::create_directories("ux0:data/vitagram");
    diag_log::set_mask(0);
    webapp_diagnostics::record(webapp_diagnostics::Stage::Button);
    assert(!std::filesystem::exists(log));
    diag_log::set_mask(1u << static_cast<unsigned>(diag_log::Kind::Performance));
    webapp_diagnostics::record(webapp_diagnostics::Stage::Count);
    assert(!std::filesystem::exists(log));
    webapp_diagnostics::record(webapp_diagnostics::Stage::ClientReady, 1);
    webapp_diagnostics::record(webapp_diagnostics::Stage::Button);
    webapp_diagnostics::record(webapp_diagnostics::Stage::NavigationFailed, -7);
    webapp_diagnostics::record(webapp_diagnostics::Stage::IpcInvalidMessage, 123);
    diag_log::set_mask(0);
    webapp_diagnostics::record(webapp_diagnostics::Stage::Ready);
    std::ifstream input(log);
    const std::string events{std::istreambuf_iterator<char>(input), {}};
    const auto normalized = std::regex_replace(events, std::regex(" at_us=[0-9]+ session_us=[0-9]+ frame=[0-9]+"), "");
    assert(normalized != events);
    assert(normalized == "stage=client_ready code=1\nstage=button code=0\nstage=navigation_failed code=-7\nstage=ipc_invalid_message code=123\n");
    unsigned color = 0;
    assert(webapp_color("#1122aB", color) && color == 0xffab2211u);
    for (const auto& value : {"#fff", "#11223344", "112233", "#1122xz", "#11223\n"}) {
        assert(!webapp_color(value, color)); assert(color == 0xffab2211u);
    }
    assert(webapp_origin("https://EXAMPLE.org:443/path#signed") == "https://example.org");
    assert(webapp_origin("https://example.org:8443?q=x") == "https://example.org:8443");
    for (const auto& value : {"file:///x", "http://example.org", "javascript:alert(1)", "https://", "https://user@example.org", "https://example.org\\@evil.org", "https://example.org\n.evil", "https://example.org\t"})
        assert(webapp_origin(value).empty());
    assert(webapp_origin("https://example.org.evil") != webapp_origin("https://example.org"));
    assert(webapp_origin("https://example.org:444") != webapp_origin("https://example.org"));
    for (bool dark : {false, true}) for (unsigned custom : {0u, 0xffe0ff00u, 0xff332211u}) {
        palette.clear();
        if (custom) { palette["surface"] = custom; palette["text"] = custom; palette["selected"] = custom; palette["accent"] = custom; }
        auto* theme = webapp_theme(dark); assert(theme && json_object_size(theme) == 16);
        auto* event = json_loads(webapp_theme_event(theme).c_str(), 0, nullptr); assert(event);
        auto* params = json_object_get(event, "theme_params"); assert(json_object_size(params) == 15);
        const char* key; json_t* value;
        json_object_foreach(params, key, value) {
            (void)key;
            assert(json_is_string(value) && json_string_length(value) == 7 && json_string_value(value)[0] == '#');
        }
        if (custom) assert(json_integer_value(json_object_get(theme, "background_color")) == (int)((custom & 255) << 16 | (custom & 0xff00) | ((custom >> 16) & 255)));
        json_decref(theme); json_decref(event);
    }
    std::puts("Mini Apps: HTTPS origins, ports and live palette conversion passed");
}
