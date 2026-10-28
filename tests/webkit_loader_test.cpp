#include "webapp_view.h"
#include "webview/module_api.h"
#include "webkit_download.h"
#include <vita2d.h>
#include <psp2/kernel/modulemgr.h>
#include <cassert>
#include <cstring>

namespace {
bool verified = false, active = false, bad_abi = false;
int loads = 0, pumps = 0, clicks = 0;
vita2d_texture pixels{};
}
bool WebKitDownload::verified() { return ::verified; }
const char* WebKitDownload::modulePath() { return "verified/webkit.suprx"; }
vita2d_texture* vita2d_create_empty_texture_format(unsigned, unsigned, SceGxmTextureFormat) { return &pixels; }
void vita2d_free_texture(vita2d_texture*) {}
void* vita2d_texture_get_datap(const vita2d_texture*) { return nullptr; }
unsigned vita2d_texture_get_stride(const vita2d_texture*) { return 0; }
int sceKernelLoadStartModule(const char* path, unsigned size, void* data, int, void*, int* status) {
    ++loads;
    assert(!std::strcmp(path, WebKitDownload::modulePath()) && size == sizeof(WebKitStart*));
    auto* copied_argument = *static_cast<WebKitStart**>(data);
    auto& start = *copied_argument;
    assert(start.abi == VITAGRAM_WEBKIT_ABI && start.size == sizeof(start));
    assert(start.host.abi == VITAGRAM_WEBKIT_ABI && start.host.size == sizeof(WebKitHost));
    assert(start.host.texture_create(1, 1, 0) == &pixels && start.host.diagnostic);
    start.api = {
        bad_abi ? VITAGRAM_WEBKIT_ABI + 1 : VITAGRAM_WEBKIT_ABI, sizeof(WebKitApi),
        [](const char* url, int same, int kind, const char* server, int port, const char* user, const char* password, int enabled) {
            assert(!std::strcmp(url, "https://example.org") && same && enabled && kind == 1);
            assert(!std::strcmp(server, "proxy") && port == 1080 && !std::strcmp(user, "user") && !std::strcmp(password, "pass"));
            active = true; return 1;
        },
        [] { active = false; }, [] { active = false; }, [] { return int(active); },
        [](unsigned, int) { ++pumps; }, [](int, int) {}, [](int x, int y, int down) { assert(x == 3 && y == 4 && down); ++clicks; },
        [](int, int) {}, [](int, int) {}, [] { return 1; }, [] { return 0; }, [](float) {},
        [](const char*) {}, [](const char*, const char*) {}, [] { return "events"; }, [] { return "link"; },
        [] { return ""; }, [] { return 0; }, [] { return &pixels; }
    };
    *status = SCE_KERNEL_START_SUCCESS; return 7;
}
int main(int argc, char**) {
    bad_abi = argc > 1;
    ProxyConfig proxy; proxy.kind = ProxyKind::Socks5; proxy.server = "proxy"; proxy.port = 1080;
    proxy.username = "user"; proxy.password = "pass";
    WebAppView view;
    assert(!view.active() && !view.texture()); view.pump(); view.close();
    assert(!view.open("https://example.org", true, true, proxy) && loads == 0);
    verified = true;
    const bool opened = view.open("https://example.org", true, true, proxy);
    assert(loads == 1);
    if (bad_abi) {
        assert(!opened && !view.error().empty() && pumps == 0);
        assert(!view.open("https://example.org", true, true, proxy) && loads == 1); return 0;
    }
    assert(opened && view.active() && view.texture() == &pixels && pumps == 1);
    view.click(3, 4, true); view.pump();
    assert(clicks == 1 && pumps == 2 && view.takeEvents() == "events" && view.takeLink() == "link");
    view.close(); assert(!view.active());
    assert(view.open("https://example.org", true, true, proxy) && loads == 1);
    view.shutdown(); assert(!view.active());
}
