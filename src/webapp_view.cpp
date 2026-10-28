#include "webapp_view.h"
#include "webview/WPEBackendVita.h"
#include "webview/ProcessProviderVita.h"
#include "memory_hygiene.h"
#include "webapp_diagnostics.h"
#include "performance.h"
#include "ui_diagnostics.h"
#include "i18n.h"
#include <WebKit/WKContext.h>
#include <WebKit/WKContextPrivate.h>
#include <WebKit/WKContextConfigurationPlayStation.h>
#include <WebKit/WKPage.h>
#include <WebKit/WKPageConfigurationRef.h>
#include <WebKit/WKPageNavigationClient.h>
#include <WebKit/WKPagePrivatePlayStation.h>
#include <WebKit/WKPreferencesRef.h>
#include <WebKit/WKPreferencesRefPrivate.h>
#include <WebKit/WKWebsiteDataStoreRef.h>
#include <WebKit/WKWebsiteDataStoreRefCurl.h>
#include <WebKit/WKUserContentControllerRef.h>
#include <WebKit/WKUserScriptRef.h>
#include <WebKit/WKView.h>
#include <WebKit/WKRunLoop.h>
#include <WebKit/WKString.h>
#include <WebKit/WKType.h>
#include <WebKit/WKURL.h>
#include <WebKit/WKURLRequest.h>
#include <WebKit/WKNavigationActionRef.h>
#include <WebKit/WKFrameInfoRef.h>
#include <WebKit/WKFramePolicyListener.h>
#include <WebKit/WKErrorRef.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>
#include <jansson.h>
#include <fstream>
#include <iterator>
#include <cstring>
#include <algorithm>

// An older staged WebKit must fail to link instead of restoring file-backed frames.
extern "C" unsigned vitagram_webkit_ram_transport_version();
extern "C" unsigned vitagram_webkit_process_isolation_version();
extern "C" unsigned vitagram_webkit_view_geometry_version();
extern "C" void vitagram_webkit_reset_trace();

namespace {
constexpr int width = 960, height = 392;
constexpr size_t max_bridge_string = 2 * 1024 * 1024;
bool engine_initialized = false;
bool engine_stopped = false;
std::string string(WKStringRef value) {
    if (!value) return {};
    auto size = WKStringGetMaximumUTF8CStringSize(value);
    if (size > max_bridge_string) return {};
    std::string result(size, '\0');
    size = WKStringGetUTF8CString(value, result.data(), size);
    result.resize(size ? size - 1 : 0); return result;
}
std::string url_string(WKURLRef value) {
    if (!value) return {};
    auto copy = WKURLCopyString(value); auto result = string(copy); WKRelease(copy); return result;
}
std::string quote(const std::string& value) {
    auto* json = json_string(value.c_str());
    if (!json) return "null";
    auto* bytes = json_dumps(json, JSON_ENCODE_ANY | JSON_COMPACT);
    std::string result = bytes ? bytes : "null"; free(bytes); json_decref(json); return result;
}
}

struct WebAppView::Impl {
    std::unique_ptr<WPEBackendVita> backend;
    WKContextRef context = nullptr;
    WKWebsiteDataStoreRef store = nullptr;
    WKViewRef view = nullptr;
    WKPageRef page = nullptr;
    vita2d_texture* pixels = nullptr;
    std::string origin, error, events, link;
    bool same_origin = true, dirty = true, polling = false, loading = true, paint_recorded = false;
    uint64_t generation = 0, next_poll = 0;
    float font_scale = 0;
    int rotation = 0, viewport_width = width, viewport_height = height;
    bool fullscreen = false;
    int pointer_x = width / 2, pointer_y = height / 2;
    struct Poll { std::weak_ptr<Impl> self; uint64_t generation; int64_t submitted; };
    bool trusted() const {
        if (!page) return false;
        auto url = WKPageCopyActiveURL(page);
        auto current = webapp_origin(url_string(url)); if (url) WKRelease(url);
        return !current.empty() && current == origin;
    }
};

WebAppView::WebAppView() : impl_(new Impl) {}
WebAppView::~WebAppView() { shutdown(); }
void WebAppView::shutdown() {
    close();
    if (engine_initialized) {
        performance::Scope measurement(performance::Metric::WebRunLoop);
        performance::SlowScope slow(performance::SlowPhase::Ui, "web_runloop");
        ui_diagnostics::Stage stage(ui_diagnostics::Phase::WebRunLoop);
        WKRunLoopIterateMain();
    }
    if (impl_->context) { WKRelease(impl_->context); impl_->context = nullptr; }
    if (impl_->store) { WKRelease(impl_->store); impl_->store = nullptr; }
}
bool WebAppView::active() const { return impl_->page; }
const std::string& WebAppView::error() const { return impl_->error; }
bool WebAppView::loading() const { return impl_->loading; }
vita2d_texture* WebAppView::texture() const { return impl_->pixels; }

bool WebAppView::open(const std::string& url, bool same_origin, bool proxy_enabled, const ProxyConfig& proxy) {
    using webapp_diagnostics::Stage;
    webapp_diagnostics::record(Stage::EngineOpen);
    close(); auto& s = *impl_;
    s.origin = webapp_origin(url); s.same_origin = same_origin; s.error.clear(); s.loading = true; s.paint_recorded = false;
    s.rotation = 0; s.fullscreen = false; s.viewport_width = width; s.viewport_height = height;
    s.pointer_x = width / 2; s.pointer_y = height / 2;
    std::string proxy_url;
    if (!webapp_proxy_url(proxy_enabled, proxy, proxy_url)) {
#ifdef VITAGRAM_WEBKIT_MODULE
        s.error = "Invalid WebKit proxy settings";
#else
        s.error = i18n::text("proxy.err.bad_server");
#endif
        return false;
    }
    if (engine_stopped) { s.error = "WebKit process stopped; restart VitaGram"; return false; }
    if (s.origin.empty()) { s.error = "HTTPS required"; return false; }
    if (vitagram_webkit_ram_transport_version() != 1) { s.error = "WebKit RAM transport unavailable"; return false; }
    if (vitagram_webkit_process_isolation_version() != 1) { s.error = "WebKit process isolation unavailable"; return false; }
    if (vitagram_webkit_view_geometry_version() != 1) { s.error = "WebKit view geometry unavailable"; return false; }
    vitagram_webkit_reset_trace();
    std::ifstream input("app0:assets/webapp-bridge.js");
    std::string bridge{std::istreambuf_iterator<char>(input), {}};
    if (bridge.empty()) { s.error = "Web App bridge unavailable"; return false; }
    static const bool initialized = [] {
        webapp_diagnostics::record(Stage::EngineInit);
        WKRunLoopInitializeMain();
        engine_initialized = register_vita_process_provider();
        webapp_diagnostics::record(Stage::EngineInitialized, engine_initialized);
        return engine_initialized;
    }();
    if (!initialized) { s.error = "WebKit initialization failed"; return false; }
    s.pixels = vita2d_create_empty_texture_format(width, height, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB);
    s.backend.reset(new WPEBackendVita(width, height));
    if (!s.pixels || !s.backend->valid()) { s.error = "WebKit surface allocation failed"; close(); return false; }
    if (vita2d_texture_get_stride(s.pixels) != width * 4) { s.error = "Unsupported WebKit surface stride"; close(); return false; }
    std::memset(vita2d_texture_get_datap(s.pixels), 0, width * height * 4);
    webapp_diagnostics::record(Stage::SurfaceReady);
    // The Vita port has one WebProcess singleton; reuse its pool across pages.
    if (!s.context) {
        webapp_diagnostics::record(Stage::ContextCreate);
        auto config = WKContextConfigurationCreate();
        WKContextConfigurationSetUserId(config, -1);
        s.context = WKContextCreateWithConfiguration(config); WKRelease(config);
        s.store = WKWebsiteDataStoreCreateNonPersistentDataStore();
        if (!s.context || !s.store) {
            if (s.context) { WKRelease(s.context); s.context = nullptr; }
            if (s.store) { WKRelease(s.store); s.store = nullptr; }
            s.error = "WebKit session allocation failed"; close(); return false;
        }
        WKContextSetPrimaryWebsiteDataStore(s.context, s.store);
        WKContextSetUsesSingleWebProcess(s.context, true);
        WKContextDisableProcessTermination(s.context);
        WKContextSetCacheModel(s.context, kWKCacheModelDocumentViewer);
    }
    webapp_diagnostics::record(Stage::ContextReady);
    if (!proxy_url.empty()) {
        auto address = WKURLCreateWithUTF8CString(proxy_url.c_str());
        auto ignore_hosts = WKStringCreateWithUTF8CString("");
        WKWebsiteDataStoreEnableCustomNetworkProxySettings(s.store, address, ignore_hosts);
        WKRelease(ignore_hosts); WKRelease(address);
    } else WKWebsiteDataStoreDisableNetworkProxySettings(s.store);
    webapp_diagnostics::record(Stage::Proxy, proxy_url.empty() ? 0 : static_cast<int>(proxy.kind));
    memory_hygiene::clear(proxy_url.data(), proxy_url.size());
    auto preferences = WKPreferencesCreate();
    WKPreferencesSetAcceleratedCompositingEnabled(preferences, false);
    WKPreferencesSetJavaScriptEnabled(preferences, true);
    WKPreferencesSetJavaScriptCanOpenWindowsAutomatically(preferences, false);
    WKPreferencesSetHyperlinkAuditingEnabled(preferences, false);
    WKPreferencesSetNotificationsEnabled(preferences, false);
    WKPreferencesSetMediaDevicesEnabled(preferences, false);
    WKPreferencesSetPeerConnectionEnabled(preferences, false);
    WKPreferencesSetWebGLEnabled(preferences, false);
    WKPreferencesSetPageCacheEnabled(preferences, false);
    auto user_content = WKUserContentControllerCreate();
    auto source = WKStringCreateWithUTF8CString(bridge.c_str());
    auto script = WKUserScriptCreateWithSource(source, kWKInjectAtDocumentStart, true);
    WKUserContentControllerAddUserScript(user_content, script); WKRelease(script); WKRelease(source);
    auto configuration = WKPageConfigurationCreate();
    WKPageConfigurationSetContext(configuration, s.context);
    WKPageConfigurationSetPreferences(configuration, preferences);
    WKPageConfigurationSetWebsiteDataStore(configuration, s.store);
    WKPageConfigurationSetUserContentController(configuration, user_content);
    webapp_diagnostics::record(Stage::ViewCreate);
    s.view = WKViewCreateWPE(s.backend->get(), configuration);
    WKRelease(configuration); WKRelease(user_content); WKRelease(preferences);
    if (!s.view) { s.error = "WebKit view allocation failed"; close(); return false; }
    webapp_diagnostics::record(Stage::ViewReady);
    WKViewSetSize(s.view, WKSizeMake(width, height));
    WKViewSetFocus(s.view, true); WKViewSetActive(s.view, true); WKViewSetVisible(s.view, true);
    s.page = WKViewGetPage(s.view);
    s.font_scale = 0;
    WKViewClientV0 view_client{}; view_client.base = {0, &s};
    view_client.setViewNeedsDisplay = [](WKViewRef, WKRect, const void* data) { static_cast<Impl*>(const_cast<void*>(data))->dirty = true; };
    WKViewSetViewClient(s.view, &view_client.base);
    WKPageNavigationClientV1 navigation{}; navigation.base = {1, &s};
    navigation.decidePolicyForNavigationAction = [](WKPageRef, WKNavigationActionRef action, WKFramePolicyListenerRef listener, WKTypeRef, const void* data) {
        auto& state = *static_cast<Impl*>(const_cast<void*>(data));
        auto request = WKNavigationActionCopyRequest(action); auto target = WKURLRequestCopyURL(request);
        const auto address = url_string(target); const auto origin = webapp_origin(address);
        auto frame = WKNavigationActionCopyTargetFrameInfo(action);
        const bool main = !frame || WKFrameInfoGetIsMainFrame(frame);
        if (frame) WKRelease(frame); WKRelease(target); WKRelease(request);
        if (origin.empty() || (main && state.same_origin && origin != state.origin)) {
            WKFramePolicyListenerIgnore(listener);
            if (main && !origin.empty()) state.link = address;
        } else WKFramePolicyListenerUse(listener);
    };
    auto fail = [](WKPageRef, WKNavigationRef, WKErrorRef error, WKTypeRef, const void* data) {
        auto& state = *static_cast<Impl*>(const_cast<void*>(data));
        // Signed launch URLs can occur in WebKit's localized error descriptions.
        webapp_diagnostics::record(Stage::NavigationFailed, WKErrorGetErrorCode(error));
        state.error = "Web App load failed (" + std::to_string(WKErrorGetErrorCode(error)) + ")"; state.loading = false;
    };
    navigation.didFailProvisionalNavigation = fail; navigation.didFailNavigation = fail;
    navigation.didStartProvisionalNavigation = [](WKPageRef, WKNavigationRef, WKTypeRef, const void* data) { webapp_diagnostics::record(Stage::NavigationStart); static_cast<Impl*>(const_cast<void*>(data))->loading = true; };
    navigation.didCommitNavigation = [](WKPageRef, WKNavigationRef, WKTypeRef, const void*) { webapp_diagnostics::record(Stage::NavigationCommit); };
    navigation.didFinishNavigation = [](WKPageRef, WKNavigationRef, WKTypeRef, const void* data) { webapp_diagnostics::record(Stage::NavigationDone); static_cast<Impl*>(const_cast<void*>(data))->loading = false; };
    navigation.webProcessDidCrash = [](WKPageRef, const void* data) { engine_stopped = true; webapp_diagnostics::record(Stage::ProcessStopped); static_cast<Impl*>(const_cast<void*>(data))->error = "WebKit process stopped; restart VitaGram"; };
    WKPageSetPageNavigationClient(s.page, &navigation.base);
    webapp_diagnostics::record(Stage::LoadRequest);
    auto address = WKURLCreateWithUTF8CString(url.c_str()); WKPageLoadURL(s.page, address); WKRelease(address);
    webapp_diagnostics::record(Stage::LoadQueued);
    s.dirty = true; return true;
}

void WebAppView::close() {
    auto& s = *impl_; ++s.generation;
    if (s.view) WKViewSetViewClient(s.view, nullptr);
    if (s.page) { WKPageSetPageNavigationClient(s.page, nullptr); WKPageStopLoading(s.page); WKPageClose(s.page); s.page = nullptr; }
    if (s.view) { WKRelease(s.view); s.view = nullptr; }
    s.backend.reset();
    if (s.pixels) {
        memory_hygiene::clear(vita2d_texture_get_datap(s.pixels), vita2d_texture_get_stride(s.pixels) * s.viewport_height);
        vita2d_free_texture(s.pixels); s.pixels = nullptr;
    }
    s.events.clear(); s.link.clear(); s.polling = false; s.next_poll = 0;
}

void WebAppView::pump() {
    if (engine_initialized) {
        performance::Scope measurement(performance::Metric::WebRunLoop);
        performance::SlowScope slow(performance::SlowPhase::Ui, "web_runloop");
        ui_diagnostics::Stage stage(ui_diagnostics::Phase::WebRunLoop);
        WKRunLoopIterateMain();
    }
    auto& s = *impl_; if (!s.page) return;
    if (s.dirty && s.pixels) {
        performance::Scope measurement(performance::Metric::WebPaint);
        performance::SlowScope slow(performance::SlowPhase::Ui, "web_paint");
        ui_diagnostics::Stage stage(ui_diagnostics::Phase::WebPaint);
        if (!s.paint_recorded) webapp_diagnostics::record(webapp_diagnostics::Stage::PaintBegin);
        WKPagePaint(s.page, static_cast<unsigned char*>(vita2d_texture_get_datap(s.pixels)), WKSizeMake(s.viewport_width, s.viewport_height), WKRectMake(0, 0, s.viewport_width, s.viewport_height));
        if (!s.paint_recorded) { webapp_diagnostics::record(webapp_diagnostics::Stage::PaintDone); s.paint_recorded = true; }
        s.dirty = false;
    }
    const auto now = sceKernelGetProcessTimeWide();
    if (s.polling || now < s.next_poll || !s.events.empty() || !s.trusted()) return;
    s.polling = true; s.next_poll = now + 100000;
    performance::Scope measurement(performance::Metric::WebBridgeSubmit);
    performance::SlowScope slow(performance::SlowPhase::Ui, "web_bridge_submit");
    ui_diagnostics::Stage stage(ui_diagnostics::Phase::WebBridge);
    auto js = WKStringCreateWithUTF8CString("window.__vitagramDrain ? window.__vitagramDrain() : ''");
    WKPageEvaluateJavaScriptInMainFrame(s.page, js, new Impl::Poll{impl_, s.generation, performance::now()}, [](WKTypeRef result, WKErrorRef error, void* context) {
        performance::Scope measurement(performance::Metric::WebBridgeReply);
        performance::SlowScope slow(performance::SlowPhase::Ui, "web_bridge_reply");
        ui_diagnostics::Stage stage(ui_diagnostics::Phase::WebBridge);
        std::unique_ptr<Impl::Poll> poll(static_cast<Impl::Poll*>(context));
        auto alive = poll->self.lock(); if (!alive) return; auto& state = *alive;
        if (state.generation != poll->generation) return;
        performance::observe(performance::Metric::WebBridgeLatency, performance::now()-poll->submitted);
        state.polling = false;
        if (!error && result && WKGetTypeID(result) == WKStringGetTypeID() && state.trusted()) {
            auto value = static_cast<WKStringRef>(result);
            if (WKStringGetMaximumUTF8CStringSize(value) > max_bridge_string) state.error = "Web App bridge response is too large";
            else state.events = string(value);
        }
    }); WKRelease(js);
}
void WebAppView::move(int x, int y) {
    if (!active()) return;
    auto& s = *impl_;
    const int display_height = s.fullscreen ? 544 : height;
    s.pointer_x = std::clamp(x, 0, width - 1); s.pointer_y = std::clamp(y, 0, display_height - 1);
    const auto [px, py] = webapp_page_point(s.rotation, s.pointer_x, s.pointer_y, width, display_height);
    WKPageHandleMouseEvent(s.page, {kWKEventMouseMove, kWKEventMouseButtonNoButton, {double(px), double(py)}, 0, 0});
}
void WebAppView::click(int x, int y, bool down) {
    if (!active()) return;
    move(x, y);
    const auto [px, py] = webapp_page_point(impl_->rotation, impl_->pointer_x, impl_->pointer_y, width, impl_->fullscreen ? 544 : height);
    WKPageHandleMouseEvent(impl_->page, {down ? kWKEventMouseDown : kWKEventMouseUp,
        kWKEventMouseButtonLeftButton, {double(px), double(py)}, 1, 0});
}
void WebAppView::scroll(int delta, int horizontal) {
    if (!active() || (!delta && !horizontal)) return;
    const auto [px, py] = webapp_page_point(impl_->rotation, impl_->pointer_x, impl_->pointer_y, width, impl_->fullscreen ? 544 : height);
    WKPageHandleWheelEvent(impl_->page, {kWKEventWheel, {double(px), double(py)}, {-double(horizontal), -double(delta)}, {}, 0});
}
int WebAppView::rotation() const { return impl_->rotation; }
bool WebAppView::fullscreen() const { return impl_->fullscreen; }
void WebAppView::orientation(int rotation, bool fullscreen) {
    auto& s = *impl_;
    if (!active() || !s.error.empty()) return;
    if (rotation != 1 && rotation != 3) rotation = 0;
    if (s.rotation == rotation && s.fullscreen == fullscreen) return;
    const int display_height = fullscreen ? 544 : height;
    const int w = rotation ? display_height : width, h = rotation ? width : display_height;
    auto* pixels = vita2d_create_empty_texture_format(w, h, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB);
    if (!pixels || vita2d_texture_get_stride(pixels) != w * 4) {
        if (pixels) vita2d_free_texture(pixels);
        s.error = "WebKit surface rotation failed"; return;
    }
    std::memset(vita2d_texture_get_datap(pixels), 0, w * h * 4);
    memory_hygiene::clear(vita2d_texture_get_datap(s.pixels), vita2d_texture_get_stride(s.pixels) * s.viewport_height);
    vita2d_free_texture(s.pixels); s.pixels = pixels;
    s.rotation = rotation; s.fullscreen = fullscreen; s.viewport_width = w; s.viewport_height = h;
    WKViewSetSize(s.view, WKSizeMake(w, h)); s.dirty = true;
    move(s.pointer_x, s.pointer_y);
}
void WebAppView::fontScale(float scale) {
    if (!active() || impl_->font_scale == scale) return;
    impl_->font_scale = scale;
    WKPageSetPageZoomFactor(impl_->page, 2 * std::clamp(scale, 0.7f, 1.3f));
}
void WebAppView::evaluate(const std::string& script) {
    if (!impl_->trusted()) return;
    performance::Scope measurement(performance::Metric::WebBridgeSubmit);
    performance::SlowScope slow(performance::SlowPhase::Ui, "web_bridge_submit");
    ui_diagnostics::Stage stage(ui_diagnostics::Phase::WebBridge);
    auto js = WKStringCreateWithUTF8CString(script.c_str()); WKPageEvaluateJavaScriptInMainFrame(impl_->page, js, nullptr, nullptr); WKRelease(js);
}
void WebAppView::event(const std::string& type, const std::string& json) {
    evaluate("window.Telegram && Telegram.WebView && Telegram.WebView.receiveEvent(" + quote(type) + "," + json + ")");
}
std::string WebAppView::takeEvents() { auto result = std::move(impl_->events); impl_->events.clear(); return result; }
std::string WebAppView::takeLink() { auto result = std::move(impl_->link); impl_->link.clear(); return result; }
