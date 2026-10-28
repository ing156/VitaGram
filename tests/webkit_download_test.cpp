#include "webkit_download.h"
#include "webview/module_api.h"
#include "https_client.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <thread>

namespace {
std::string mode, fixtures;
int metadata_calls = 0, package_calls = 0;
bool fail_thread = false;
std::string read(const std::string& path) { std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}}; }
WebKitDownload::Snapshot wait(WebKitDownload& download) {
    for (int i = 0; i < 1000; ++i) {
        const auto state = download.snapshot();
        if (!state.busy()) return state;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(false && "download worker did not finish"); return {};
}
}
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*entry)(void*), void* argument) {
    size_t stack_size = 0;
    assert(attr && pthread_attr_getstacksize(attr, &stack_size) == 0 && stack_size == 256 * 1024);
    if (fail_thread) { fail_thread = false; return EAGAIN; }
    return __real_pthread_create(thread, attr, entry, argument);
}
namespace update {
HttpsResult https_get(const std::string& url, const std::string&, const std::string&,
    const std::function<bool(const char*, size_t)>& sink, const std::atomic<bool>* cancel,
    const std::function<void(int64_t)>&) {
    HttpsResult result; result.status = 200;
    std::string data;
    if (url.find("api.github.com") != std::string::npos) {
        ++metadata_calls;
        if (mode == "offline") { result.error = "offline"; return result; }
        if (mode == "missing") data = "{}";
        else data = "{\"tag_name\":\"" VITAGRAM_WEBKIT_ID "\",\"assets\":[{\"name\":\"vitagram-" VITAGRAM_WEBKIT_ID ".zip\",\"size\":" +
            std::to_string(read(fixtures + "/addon.zip").size()) + ",\"browser_download_url\":\"https://example.org/addon.zip\"},"
            "{\"name\":\"vitagram-" VITAGRAM_WEBKIT_ID ".zip.sig\",\"browser_download_url\":\"https://example.org/addon.sig\"}]}";
    } else if (url.find(".sig") != std::string::npos) data = read(fixtures + "/addon.sig");
    else { ++package_calls; data = read(fixtures + "/addon.zip"); if (mode == "tampered") data[data.size() / 2] ^= 1; }
    if (mode == "cancel" && package_calls) const_cast<std::atomic<bool>*>(cancel)->store(true);
    if (cancel->load() || !sink(data.data(), data.size())) result.error = "cancelled";
    return result;
}
}

int main(int argc, char** argv) {
    assert(argc == 3); fixtures = argv[1]; mode = argv[2];
    using State = WebKitDownload::State;
    WebKitDownload download;
    assert(!WebKitDownload::verified());
    if (mode == "thread-error") {
        fail_thread = true;
        download.prepare();
        assert(download.snapshot().state == State::Failed && !WebKitDownload::verified());
        assert(metadata_calls == 0);
        download.install();
        assert(wait(download).state == State::Available);
        return 0;
    }
    download.prepare(); auto state = wait(download);
    if (mode == "missing" || mode == "offline") {
        assert(state.state == State::Failed && package_calls == 0 && !WebKitDownload::verified()); return 0;
    }
    assert(state.state == State::Available && state.total && package_calls == 0);
    download.install(); state = wait(download);
    if (mode == "cancel") { assert(state.state == State::Idle && !WebKitDownload::verified()); return 0; }
    if (mode == "tampered" || mode == "incompatible") {
        assert(state.state == State::Failed && !WebKitDownload::verified());
        assert(read(WebKitDownload::modulePath()).empty()); return 0;
    }
    assert(state.state == State::Ready && WebKitDownload::verified());
    assert(read(WebKitDownload::modulePath()) == "TEST MODULE");
    // A later launch re-verifies the signed archive and repairs a modified extracted module offline.
    { std::ofstream file(WebKitDownload::modulePath()); file << "modified"; }
    WebKitDownload reopen; mode = "offline"; reopen.prepare(); state = wait(reopen);
    assert(state.state == State::Ready && metadata_calls == 1 && package_calls == 1);
    assert(read(WebKitDownload::modulePath()) == "TEST MODULE");
    std::puts("WebKit add-on: consent, signed install, compatibility and offline re-verification passed");
}
