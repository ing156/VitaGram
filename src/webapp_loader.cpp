#include "webapp_view.h"
#include "webview/module_api.h"
#include "webkit_download.h"
#include "webapp_diagnostics.h"
#include "memory_hygiene.h"
#include <psp2/kernel/modulemgr.h>
#include <vita2d.h>

namespace {
WebKitStart engine{};
int module_id = -1;
bool load_attempted = false;
bool loaded() { return module_id >= 0 && webkit_api_valid(engine.api); }
}
struct WebAppView::Impl { std::string error; };
WebAppView::WebAppView() : impl_(new Impl) {}
WebAppView::~WebAppView() { shutdown(); }
bool WebAppView::open(const std::string& url, bool same, bool enabled, const ProxyConfig& proxy) {
    impl_->error.clear();
    if (!WebKitDownload::verified()) { impl_->error = "WebKit has not been verified"; return false; }
    if (!load_attempted) {
        load_attempted = true;
        engine.abi = VITAGRAM_WEBKIT_ABI; engine.size = sizeof(engine);
        engine.host = {VITAGRAM_WEBKIT_ABI, sizeof(WebKitHost),
            [](unsigned w, unsigned h, unsigned format) { return vita2d_create_empty_texture_format(w, h, static_cast<SceGxmTextureFormat>(format)); },
            vita2d_free_texture, vita2d_texture_get_datap, vita2d_texture_get_stride,
            [](unsigned stage, int64_t code) { webapp_diagnostics::record(static_cast<webapp_diagnostics::Stage>(stage), code); }};
        int status = -1;
        // Module startup may copy its arguments; return the API through a persistent host pointer.
        auto* start = &engine;
        module_id = sceKernelLoadStartModule(WebKitDownload::modulePath(), sizeof(start), &start, 0, nullptr, &status);
        if (status != SCE_KERNEL_START_SUCCESS) module_id = -1;
    }
    if (!loaded()) { impl_->error = "WebKit module failed to start; restart VitaGram"; return false; }
    engine.api.pump(diag_log::mask().load(), memory_hygiene::enabled());
    return engine.api.open(url.c_str(), same, static_cast<int>(proxy.kind), proxy.server.c_str(), proxy.port,
        proxy.username.c_str(), proxy.password.c_str(), enabled);
}
void WebAppView::close() { if (loaded()) engine.api.close(); impl_->error.clear(); }
void WebAppView::shutdown() { if (loaded()) engine.api.shutdown(); }
bool WebAppView::active() const { return loaded() && engine.api.active(); }
void WebAppView::pump() {
    if (loaded()) {
        performance::Scope measurement(performance::Metric::WebRunLoop);
        engine.api.pump(diag_log::mask().load(), memory_hygiene::enabled());
    }
}
void WebAppView::move(int x, int y) { if (loaded()) engine.api.move(x, y); }
void WebAppView::click(int x, int y, bool down) { if (loaded()) engine.api.click(x, y, down); }
void WebAppView::scroll(int dy, int dx) { if (loaded()) engine.api.scroll(dy, dx); }
void WebAppView::orientation(int rotation, bool full) { if (loaded()) engine.api.orientation(rotation, full); }
int WebAppView::rotation() const { return loaded() ? engine.api.rotation() : 0; }
bool WebAppView::fullscreen() const { return loaded() && engine.api.fullscreen(); }
void WebAppView::fontScale(float scale) { if (loaded()) engine.api.font_scale(scale); }
void WebAppView::evaluate(const std::string& js) { if (loaded()) engine.api.evaluate(js.c_str()); }
void WebAppView::event(const std::string& type, const std::string& json) { if (loaded()) engine.api.event(type.c_str(), json.c_str()); }
std::string WebAppView::takeEvents() { return loaded() ? engine.api.take_events() : ""; }
std::string WebAppView::takeLink() { return loaded() ? engine.api.take_link() : ""; }
const std::string& WebAppView::error() const {
    if (loaded()) impl_->error = engine.api.error();
    return impl_->error;
}
bool WebAppView::loading() const { return loaded() && engine.api.loading(); }
vita2d_texture* WebAppView::texture() const { return loaded() ? engine.api.texture() : nullptr; }
