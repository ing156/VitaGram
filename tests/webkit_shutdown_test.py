"""Compile the production shutdown branches with small IPC/platform stand-ins."""
import os
from pathlib import Path
import subprocess
import sys

source, build = map(Path, sys.argv[1:])
aux = (source / 'Source/WebKit/Shared/AuxiliaryProcess.cpp').read_text()
aux = aux[aux.index('void AuxiliaryProcess::didClose('):aux.index('void AuxiliaryProcess::initialize(')]
web = (source / 'Source/WebKit/WebProcess/WebProcess.cpp').read_text()
web = web[web.index('void WebProcess::initializeConnection('):]
web = web[web.index('AuxiliaryProcess::initializeConnection(connection);') + len('AuxiliaryProcess::initializeConnection(connection);'):web.index('    protectedEventDispatcher()')]
network = (source / 'Source/WebKit/NetworkProcess/NetworkProcess.cpp').read_text()
network = network[network.index('void NetworkProcess::initializeConnection('):]
network = network[network.index('AuxiliaryProcess::initializeConnection(connection);') + len('AuxiliaryProcess::initializeConnection(connection);'):network.index('    for (auto& supplement')]
fixture = r'''
#include <cassert>
#include <cstdlib>
#include <functional>
#include <vector>
#define PLATFORM(x) PLATFORM_##x
#define ENABLE(x) 0
namespace IPC {
struct Connection {
    struct Identifier { };
    using DidCloseOnConnectionWorkQueueCallback = void(*)(Connection*);
    DidCloseOnConnectionWorkQueueCallback closed = nullptr;
    bool exitOnFailure = false;
    void setDidCloseOnConnectionWorkQueueCallback(DidCloseOnConnectionWorkQueueCallback f) { closed = f; }
    void setShouldExitOnSyncMessageSendFailure(bool value) { exitOnFailure = value; }
};
}
[[maybe_unused]] static void callExit(IPC::Connection*) { }
[[maybe_unused]] static void crashAfter10Seconds(IPC::Connection*) { }
[[maybe_unused]] static void callExitSoon(IPC::Connection*) { }
static unsigned exits = 0;
[[maybe_unused]] static void terminateProcess(int) { ++exits; }
struct AuxiliaryProcess {
    unsigned stops = 0;
    void stopRunLoop() { ++stops; }
    void didClose(IPC::Connection&);
};
static std::vector<std::function<void()>> pending;
struct RunLoop {
    static RunLoop& mainSingleton() { static RunLoop loop; return loop; }
    void dispatch(std::function<void()> callback) { pending.push_back(std::move(callback)); }
};
template<class T> struct Ref {
    T* value;
    Ref(T& value) : value(&value) { }
    T* operator->() const { return value; }
};
'''
fixture += aux + '\nstatic void initializeWeb(IPC::Connection* connection) {' + web + '}\n'
fixture += 'static void initializeNetwork(IPC::Connection* connection) {' + network + '}\n'
launcher = (source / 'Source/WebKit/UIProcess/Launcher/playstation/ProcessLauncherPlayStation.cpp').read_text()
failure = launcher[launcher.index('    if (appLocalPid < 0) {'):]
failure = failure[failure.index('#if PLATFORM(VITA)'):failure.index('        return;')]
fixture += '''
struct ProcessLauncher {
    unsigned callbacks = 0;
    void didFinishLaunchingProcess(int id, IPC::Connection::Identifier) { assert(id == 0); ++callbacks; }
    void failLaunch() {
''' + failure + '} };\n'
geometry = (source / 'Source/WebKit/UIProcess/playstation/PlayStationWebView.cpp').read_text()
geometry = geometry[geometry.index('void PlayStationWebView::setViewSize('):geometry.index('void PlayStationWebView::setViewState(')]
fixture += r'''
namespace WebCore { struct IntSize { int width = 0, height = 0; }; }
struct DrawingArea {
    WebCore::IntSize size;
    void setSize(WebCore::IntSize value) { size = value; }
};
struct Page {
    DrawingArea* area;
    DrawingArea* drawingArea() { return area; }
};
struct PlayStationWebView {
    Page* m_page;
    WebCore::IntSize m_viewSize;
    void setViewSize(WebCore::IntSize);
};
'''
fixture += geometry
curl = (source / 'Source/WebCore/platform/network/curl/CurlRequest.cpp').read_text()
curl = curl[curl.index('void CurlRequest::didCompleteTransfer('):]
curl = curl[:curl.index('    bool isProxyAuthenticationRequired')]
fixture += r'''
using CURLcode = int;
constexpr int CURLE_OK = 0;
static unsigned traceCount = 0, errorCount = 0, lastError = 0;
[[maybe_unused]] static void vita_stream_browser_trace(const char*) { ++traceCount; }
[[maybe_unused]] static void vita_stream_browser_trace_value(const char*, unsigned value) { ++errorCount; lastError = value; }
struct CurlRequest {
    bool cancelled = false;
    unsigned cancels = 0;
    bool isCancelled() { return cancelled; }
    void didCancelTransfer() { ++cancels; }
    void didCompleteTransfer(CURLcode);
};
'''
fixture += curl + '}\n'
dns = (source / 'Source/WebCore/platform/network/curl/CurlContext.cpp').read_text()
dns = dns[dns.index('        auto resolveURL = url;'):]
dns = dns[:dns.index('        if (!host.isEmpty() && port)')]
fixture += r'''
#include <optional>
#include <string>
struct URL {
    std::string name;
    std::string host() const { return name; }
    std::optional<unsigned> port() const { return 1080; }
    int protocol() const { return 0; }
};
namespace WTF { static unsigned defaultPortForProtocol(int) { return 80; } }
struct CurlProxySettings {
    enum class Mode { Default, NoProxy, Custom };
    Mode value = Mode::NoProxy;
    Mode mode() const { return value; }
    std::string url() const { return "proxy.example"; }
};
struct CurlContext {
    CurlProxySettings proxy;
    static CurlContext& singleton() { static CurlContext context; return context; }
    const CurlProxySettings& proxySettings() const { return proxy; }
};
static std::string dnsHost(const URL& url) {
''' + dns + 'return host; }\n'
fixture += r'''
int main() {
    for (auto mode : {CurlProxySettings::Mode::Default, CurlProxySettings::Mode::NoProxy, CurlProxySettings::Mode::Custom}) {
        CurlContext::singleton().proxy.value = mode;
        assert(dnsHost(URL { "site.example" }) == (mode == CurlProxySettings::Mode::Custom ? "proxy.example" : "site.example"));
    }
    CurlRequest request;
    request.cancelled = true;
    request.didCompleteTransfer(60);
    assert(request.cancels == 1 && traceCount == 0 && errorCount == 0);
    request.cancelled = false;
    request.didCompleteTransfer(CURLE_OK);
    request.didCompleteTransfer(60);
#if PLATFORM(VITA)
    assert(traceCount == 2 && errorCount == 1 && lastError == 60);
#else
    assert(traceCount == 0 && errorCount == 0);
#endif
    DrawingArea area;
    Page page { &area };
    PlayStationWebView view { &page, { } };
    view.setViewSize({960, 392});
    assert(view.m_viewSize.width == 960 && view.m_viewSize.height == 392);
#if PLATFORM(VITA)
    assert(area.size.width == 960 && area.size.height == 392);
    page.area = nullptr;
    view.setViewSize({400, 300});
    page.area = &area;
    view.setViewSize({400, 300});
    assert(area.size.width == 400 && area.size.height == 300);
#else
    assert(area.size.width == 0 && area.size.height == 0);
#endif
    IPC::Connection web, network;
    initializeWeb(&web); initializeNetwork(&network);
    AuxiliaryProcess process;
    process.didClose(web);
    ProcessLauncher launcher;
    launcher.failLaunch();
    assert(launcher.callbacks == 0);
    for (auto& callback : pending) callback();
#if PLATFORM(VITA)
    assert(launcher.callbacks == 1);
    assert(process.stops == 1 && exits == 0);
    assert(!web.closed && !network.closed && !web.exitOnFailure);
#else
    assert(launcher.callbacks == 0);
    assert(network.closed == callExitSoon);
#if PLATFORM(GTK) || PLATFORM(WPE)
    assert(web.closed == crashAfter10Seconds && !web.exitOnFailure);
    assert(process.stops == 1 && exits == 0);
#else
    assert(web.closed == callExit && web.exitOnFailure);
    assert(process.stops == 0 && exits == 1);
#endif
#endif
}
'''
path = build / 'shutdown.cpp'
path.write_text(fixture)
for platform in ('VITA', 'GTK', 'WPE', 'OTHER'):
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', *[f'-DPLATFORM_{name}={int(name == platform)}'
                        for name in ('VITA', 'GTK', 'WPE')], str(path), '-o', str(build / 'shutdown')], check=True)
    subprocess.run([str(build / 'shutdown')], check=True)
threading = (source / 'Source/WTF/wtf/Threading.cpp').read_text()
assert ', auxiliaryProcessType(processType())' in threading
assert 'setAuxiliaryProcessType(*context->auxiliaryProcessType)' in threading
print('WebKit: Vita geometry reaches the drawing area; safe closure and asynchronous launch failure; other platforms preserved')
