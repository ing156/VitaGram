#include "webkit_download.h"
#include "webview/module_api.h"
#include "https_client.h"
#include "update_core.h"
#include <jansson.h>
#include <cstdio>
#include <sys/stat.h>
#ifdef __vita__
#include <psp2/io/devctl.h>
#endif

namespace {
std::atomic<bool> prepared{false};
constexpr uint64_t max_package = 96ull << 20, max_unpacked = 160ull << 20;
const char directory[] = "ux0:data/vitagram/webkit/" VITAGRAM_WEBKIT_ID;
const char release_url[] = "https://api.github.com/repos/ing156/VitaGram/releases/tags/" VITAGRAM_WEBKIT_ID;
const char asset_name[] = "vitagram-" VITAGRAM_WEBKIT_ID ".zip";
const char ca_file[] = "app0:certs/cacert.pem";
std::string path(const char* name) { return std::string(directory) + "/" + name; }
bool regular(const std::string& name) {
    struct stat st{}; return lstat(name.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool read_small(const std::string& name, size_t limit, std::string& result) {
    result.clear();
    FILE* file = std::fopen(name.c_str(), "rb");
    if (!file) return false;
    char buffer[1024]; size_t count;
    bool ok = true;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file))) {
        if (result.size() + count > limit) { ok = false; break; }
        result.append(buffer, count);
    }
    ok = ok && !std::ferror(file); std::fclose(file); return ok;
}
}

const char* WebKitDownload::modulePath() { return "ux0:data/vitagram/webkit/" VITAGRAM_WEBKIT_ID "/ready/webkit.suprx"; }
bool WebKitDownload::verified() { return prepared.load(); }
WebKitDownload::~WebKitDownload() { cancel_ = true; if (thread_started_) pthread_join(thread_, nullptr); }
WebKitDownload::Snapshot WebKitDownload::snapshot() const { std::lock_guard<std::mutex> lock(mutex_); return state_; }
void WebKitDownload::fail(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.state = cancel_ ? State::Idle : State::Failed; state_.error = error;
}
void WebKitDownload::cancel() { if (snapshot().busy()) cancel_ = true; }
void WebKitDownload::prepare() { if (snapshot().state == State::Idle) start(false); }
void WebKitDownload::install() {
    const auto state = snapshot().state;
    if (state == State::Available) start(true);
    else if (state == State::Failed) start(false);
}
void WebKitDownload::start(bool download) {
    if (snapshot().busy()) return;
    if (thread_started_) { pthread_join(thread_, nullptr); thread_started_ = false; }
    cancel_ = false; downloading_ = download;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.state = download ? State::Downloading : State::Checking;
        state_.bytes = 0; state_.error.clear();
    }
    pthread_attr_t attr;
    int result = pthread_attr_init(&attr);
    if (result == 0) {
        // Vita defaults to 32 KiB; archive verification, TLS and zlib need the updater's larger stack.
        result = pthread_attr_setstacksize(&attr, 256 * 1024);
        if (result == 0) result = pthread_create(&thread_, &attr, [](void* data) -> void* {
            auto* self = static_cast<WebKitDownload*>(data); self->run(self->downloading_); return nullptr;
        }, this);
        pthread_attr_destroy(&attr);
    }
    thread_started_ = result == 0;
    if (!thread_started_) fail("Cannot start WebKit download worker");
}

bool WebKitDownload::unpack() {
    // shortcut: re-extract the verified archive once per launch; add signed module digests if device I/O is too slow.
    std::string signature;
    if (!read_small(path("engine.sig"), 1024, signature)) return false;
    FILE* file = std::fopen(path("engine.zip").c_str(), "rb");
    if (!file) return false;
    update::SignatureVerifier verifier;
    bool ok = verifier.begin(VITAGRAM_WEBKIT_ID, nullptr, update::SignatureVerifier::Kind::WebKit);
    char buffer[64 * 1024]; size_t count; uint64_t total = 0;
    while (ok && !cancel_ && (count = std::fread(buffer, 1, sizeof(buffer), file))) {
        total += count;
        if (total > max_package) { ok = false; break; }
        verifier.update(buffer, count);
    }
    ok = ok && !cancel_ && !std::ferror(file) && verifier.finish(reinterpret_cast<const unsigned char*>(signature.data()), signature.size());
    if (ok) {
        std::rewind(file);
        ok = update::remove_tree(path("stage"));
        std::string error;
        if (ok) ok = update::extract_zip(file, path("stage"), [&](uint64_t, uint64_t size) { return !cancel_ && size <= max_unpacked; }, error);
        std::string compatibility;
        ok = ok && read_small(path("stage/compatibility"), 64, compatibility) &&
            compatibility == VITAGRAM_WEBKIT_ID "\n" && regular(path("stage/webkit.suprx")) &&
            regular(path("stage/licenses/WebCore/LICENSE-LGPL-2.1"));
        if (ok && !cancel_) ok = update::remove_tree(path("ready")) && std::rename(path("stage").c_str(), path("ready").c_str()) == 0;
        else ok = false;
    }
    std::fclose(file);
    if (!ok) update::remove_tree(path("stage"));
    if (ok) prepared = true;
    return ok;
}

void WebKitDownload::run(bool download) {
    if (!update::make_directories(directory)) { fail("Cannot create WebKit directory"); return; }
    if (!download) {
        if (unpack()) { std::lock_guard<std::mutex> lock(mutex_); state_.state = State::Ready; return; }
        if (cancel_) { fail("Cancelled"); return; }
        std::string json;
        const auto http = update::https_get(release_url, ca_file, "application/vnd.github+json", [&](const char* data, size_t size) {
            if (json.size() + size > 512 * 1024) return false;
            json.append(data, size); return true;
        }, &cancel_);
        if (!http.error.empty() || cancel_) { fail(http.error); return; }
        auto* root = json_loads(json.c_str(), JSON_REJECT_DUPLICATES, nullptr);
        const auto* tag = json_string_value(json_object_get(root, "tag_name"));
        bool valid = tag && std::string(tag) == VITAGRAM_WEBKIT_ID && !json_is_true(json_object_get(root, "draft"));
        std::string url, sig; uint64_t size = 0;
        size_t index; json_t* asset;
        json_array_foreach(json_object_get(root, "assets"), index, asset) {
            const auto* name = json_string_value(json_object_get(asset, "name"));
            const auto* address = json_string_value(json_object_get(asset, "browser_download_url"));
            update::Url parsed;
            if (!name || !address || !update::split_https_url(address, parsed)) continue;
            if (std::string(name) == asset_name) {
                if (!url.empty()) valid = false;
                url = address;
                const auto length = json_integer_value(json_object_get(asset, "size"));
                if (length <= 0 || uint64_t(length) > max_package) valid = false;
                else size = uint64_t(length);
            } else if (std::string(name) == std::string(asset_name) + ".sig") {
                if (!sig.empty()) valid = false;
                sig = address;
            }
        }
        json_decref(root);
        if (!valid || url.empty() || sig.empty() || !size) { fail("Compatible signed WebKit add-on is not published"); return; }
        url_ = url; signature_url_ = sig;
        std::lock_guard<std::mutex> lock(mutex_);
        state_.total = size; state_.state = cancel_ ? State::Idle : State::Available; return;
    }
#ifdef __vita__
    SceIoDevInfo info{};
    if (sceIoDevctl("ux0:", 0x3001, nullptr, 0, &info, sizeof(info)) >= 0 &&
        info.free_size >= 0 && uint64_t(info.free_size) < max_unpacked + snapshot().total + (16ull << 20)) {
        fail("Not enough free space for WebKit"); return;
    }
#endif
    std::string signature;
    auto http = update::https_get(signature_url_, ca_file, "application/octet-stream", [&](const char* data, size_t size) {
        if (signature.size() + size > 1024) return false;
        signature.append(data, size); return true;
    }, &cancel_);
    if (!http.error.empty() || cancel_ || signature.empty()) { fail("Cannot download WebKit signature"); return; }
    FILE* file = std::fopen(path("download.part").c_str(), "wb");
    if (!file) { fail("Cannot write WebKit download"); return; }
    update::SignatureVerifier verifier;
    bool ok = verifier.begin(VITAGRAM_WEBKIT_ID, nullptr, update::SignatureVerifier::Kind::WebKit);
    uint64_t received = 0; const auto expected = snapshot().total;
    if (ok) http = update::https_get(url_, ca_file, "application/octet-stream", [&](const char* data, size_t size) {
        if (received + size > max_package || received + size > expected || std::fwrite(data, 1, size, file) != size) return false;
        verifier.update(data, size); received += size;
        std::lock_guard<std::mutex> lock(mutex_); state_.bytes = received; return true;
    }, &cancel_);
    ok = std::fclose(file) == 0 && ok && !cancel_ && http.error.empty() && received == expected &&
        verifier.finish(reinterpret_cast<const unsigned char*>(signature.data()), signature.size());
    if (ok) {
        file = std::fopen(path("signature.part").c_str(), "wb");
        if (!file) ok = false;
        else { ok = std::fwrite(signature.data(), 1, signature.size(), file) == signature.size(); ok = std::fclose(file) == 0 && ok; }
    }
    if (ok) ok = std::rename(path("download.part").c_str(), path("engine.zip").c_str()) == 0 &&
        std::rename(path("signature.part").c_str(), path("engine.sig").c_str()) == 0 && unpack();
    std::remove(path("download.part").c_str()); std::remove(path("signature.part").c_str());
    if (!ok) { fail("WebKit download, signature verification or installation failed"); return; }
    std::lock_guard<std::mutex> lock(mutex_); state_.state = State::Ready;
}
