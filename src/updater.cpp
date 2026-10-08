#include "updater.h"
#include "https_client.h"
#include "update_core.h"
#include <cstdio>
#include <vector>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>
#endif

#ifndef VITAGRAM_UPDATE_REPO
#define VITAGRAM_UPDATE_REPO "ing156/VitaGram"
#endif

namespace {
const char kLatestRelease[] = "https://api.github.com/repos/" VITAGRAM_UPDATE_REPO "/releases/latest";
constexpr size_t kMaxJson = 512 * 1024;
constexpr size_t kMaxSignature = 1024;
constexpr uint64_t kMaxPackage = 256ull << 20;
}

Updater::Updater(std::string current_version, std::string ca_file, std::string work_dir)
    : current_(std::move(current_version)), ca_file_(std::move(ca_file)), work_dir_(std::move(work_dir)) {}

Updater::~Updater() {
    cancel_ = true;
    finishThread();
}

Updater::Snapshot Updater::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}

void Updater::set(State state) {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.state = state;
    snap_.percent = 0; snap_.bytes = snap_.total = 0;
}

void Updater::fail(Error error, const std::string& detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.state = State::Failed;
    snap_.error = error;
    snap_.detail = detail;
}

bool Updater::finishThread() {
    if (!thread_started_) return true;
    pthread_join(thread_, nullptr);
    thread_started_ = false;
    return true;
}

bool Updater::start(void* (*entry)(void*)) {
    finishThread(); // the previous run has ended (state is not busy); reclaim its thread
    cancel_ = false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024); // TLS handshake + zlib need more than the default
    int rc = pthread_create(&thread_, &attr, entry, this);
    pthread_attr_destroy(&attr);
    thread_started_ = rc == 0;
    return thread_started_;
}

void Updater::check() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snap_.busy() || snap_.state == State::Available || snap_.state == State::Installed) return;
        snap_ = Snapshot();
        snap_.state = State::Checking;
    }
    if (!start(&Updater::checkEntry)) fail(Error::Network, "cannot start the update thread");
}

void Updater::install() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snap_.state != State::Available) return;
        snap_.state = State::Downloading;
        snap_.percent = 0; snap_.bytes = snap_.total = 0;
    }
    if (!start(&Updater::installEntry)) fail(Error::Install, "cannot start the update thread");
}

void Updater::cancel() { cancel_ = true; }

void* Updater::checkEntry(void* self) { static_cast<Updater*>(self)->runCheck(); return nullptr; }
void* Updater::installEntry(void* self) { static_cast<Updater*>(self)->runInstall(); return nullptr; }

void Updater::runCheck() {
    std::string json;
    update::HttpsResult http = update::https_get(kLatestRelease, ca_file_, "application/vnd.github+json",
        [&](const char* data, size_t size) {
            if (json.size() + size > kMaxJson) return false;
            json.append(data, size);
            return true;
        }, &cancel_);
    if (cancel_) { fail(Error::Cancelled, "cancelled"); return; }
    if (!http.error.empty()) {
        // 404 = the repository has no published release yet (or is private): not a network fault.
        fail(http.status == 404 ? Error::NoRelease : Error::Network, http.error);
        return;
    }
    update::Release release;
    std::string error;
    if (!update::parse_release(json, release, error)) { fail(Error::NoRelease, error); return; }
    if (update::compare_versions(current_, release.version) >= 0) { set(State::UpToDate); return; }
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.version = release.version;
    vpk_url_ = release.vpk_url; sig_url_ = release.sig_url; vpk_name_ = release.vpk_name; vpk_size_ = release.vpk_size;
    snap_.state = State::Available;
}

void Updater::runInstall() {
    std::string version, vpk_url, sig_url;
    uint64_t expected = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        version = snap_.version; vpk_url = vpk_url_; sig_url = sig_url_; expected = vpk_size_;
    }
    if (expected > kMaxPackage) { fail(Error::Package, "package is unreasonably large"); return; }
    const std::string vpk_path = work_dir_ + "/update.vpk", package_dir = work_dir_ + "/pkg";
    update::remove_tree(package_dir);
    if (!update::make_directories(work_dir_)) { fail(Error::Storage, "cannot create " + work_dir_); return; }

    // The signature is small: fetch it first so a bad release fails before a long download.
    std::vector<unsigned char> signature;
    update::HttpsResult http = update::https_get(sig_url, ca_file_, "application/octet-stream",
        [&](const char* data, size_t size) {
            if (signature.size() + size > kMaxSignature) return false;
            signature.insert(signature.end(), data, data + size);
            return true;
        }, &cancel_);
    if (cancel_) { fail(Error::Cancelled, "cancelled"); return; }
    if (!http.error.empty()) { fail(Error::Network, http.error); return; }

    update::SignatureVerifier verifier;
    if (!verifier.begin(version)) { fail(Error::BadSignature, "cannot load the signing key"); return; }
    FILE* file = std::fopen(vpk_path.c_str(), "wb");
    if (!file) { fail(Error::Storage, "cannot write " + vpk_path); return; }
    uint64_t received = 0;
    bool write_failed = false;
    http = update::https_get(vpk_url, ca_file_, "application/octet-stream",
        [&](const char* data, size_t size) {
            if (received + size > kMaxPackage) return false;
            if (std::fwrite(data, 1, size, file) != size) { write_failed = true; return false; }
            verifier.update(data, size);
            received += size;
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.bytes = received;
            snap_.percent = snap_.total ? (int)(received * 100 / snap_.total) : 0;
            return true;
        }, &cancel_,
        [&](int64_t length) {
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.total = length > 0 ? (uint64_t)length : expected;
        });
    bool closed = std::fclose(file) == 0;
    if (write_failed || !closed) { std::remove(vpk_path.c_str()); fail(Error::Storage, "disk write failed (storage full?)"); return; }
    if (cancel_) { std::remove(vpk_path.c_str()); fail(Error::Cancelled, "cancelled"); return; }
    if (!http.error.empty()) { std::remove(vpk_path.c_str()); fail(Error::Network, http.error); return; }

    set(State::Verifying);
    if (!verifier.finish(signature.data(), signature.size())) {
        std::remove(vpk_path.c_str());
        fail(Error::BadSignature, "the package signature does not match");
        return;
    }

    set(State::Extracting);
    std::string error;
    bool extracted = update::extract_zip(vpk_path, package_dir, [&](uint64_t done, uint64_t total) {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.bytes = done; snap_.total = total;
        snap_.percent = total ? (int)(done * 100 / total) : 0;
        return !cancel_;
    }, error);
    if (!extracted) {
        update::remove_tree(package_dir);
        std::remove(vpk_path.c_str());
        fail(cancel_ ? Error::Cancelled : Error::Package, error);
        return;
    }

    std::string detail;
    if (!stageHead(package_dir, detail)) {
        update::remove_tree(package_dir);
        fail(Error::Install, detail + " (" + vpk_path + ")");
        return;
    }
    // Stage only: applyPending() installs at the next start. The marker is written last.
    FILE* marker = std::fopen((work_dir_ + "/pending").c_str(), "wb");
    if (!marker || std::fclose(marker) != 0) { fail(Error::Storage, "cannot write the install marker"); return; }
    set(State::Installed); // the verified VPK stays until applyPending() succeeds, for a manual install
}

Updater::Pending Updater::applyPending(const std::string& work_dir, std::string& detail) {
    const std::string marker = work_dir + "/pending", package_dir = work_dir + "/pkg", result = work_dir + "/result.txt";
    FILE* found = std::fopen(marker.c_str(), "rb");
    if (!found) return Pending::None;
    std::fclose(found);
    std::remove(marker.c_str()); // one attempt only: a failure must not loop on every start
    std::remove(result.c_str());
    bool ok = promote(package_dir, detail);
    update::remove_tree(package_dir);
    if (!ok) {
        if (FILE* f = std::fopen(result.c_str(), "wb")) { std::fwrite(detail.data(), 1, detail.size(), f); std::fclose(f); }
        return Pending::Failed;
    }
    std::remove((work_dir + "/update.vpk").c_str());
    return Pending::Applied;
}

void Updater::reportStartupFailure(const std::string& detail) {
    fail(Error::Install, detail);
}

#ifdef __vita__
bool Updater::stageHead(const std::string& package_dir, std::string& detail) {
    // The promoter wants sce_sys/package/head.bin, which only the console's own installer creates.
    // The installed copy of this very app has one for the same title id, so reuse it.
    const std::string head_dir = package_dir + "/sce_sys/package", head_path = head_dir + "/head.bin";
    FILE* source = std::fopen("app0:sce_sys/package/head.bin", "rb");
    if (!source) { detail = "app0:sce_sys/package/head.bin is missing"; return false; }
    if (!update::make_directories(head_dir)) { std::fclose(source); detail = "cannot create " + head_dir; return false; }
    FILE* target = std::fopen(head_path.c_str(), "wb");
    if (!target) { std::fclose(source); detail = "cannot write " + head_path; return false; }
    char buffer[4096];
    size_t n;
    bool copied = true;
    while ((n = std::fread(buffer, 1, sizeof(buffer), source)) > 0) if (std::fwrite(buffer, 1, n, target) != n) copied = false;
    std::fclose(source);
    if (std::fclose(target) != 0 || !copied) { detail = "cannot write " + head_path; return false; }
    return true;
}

bool Updater::promote(const std::string& package_dir, std::string& detail) {
    int rc = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (rc < 0) { char text[48]; std::snprintf(text, sizeof(text), "promoter module: 0x%08X", rc); detail = text; return false; }
    bool ok = false;
    rc = scePromoterUtilityInit();
    if (rc >= 0) {
        rc = scePromoterUtilityPromotePkgWithRif(package_dir.c_str(), 1);
        if (rc >= 0) {
            int result = 0;
            for (int waited = 0; waited < 600; ++waited) { // up to 60 s, until the promoter reports a result
                int state = 0;
                if (scePromoterUtilityGetState(&state) >= 0 && state == 0) break;
                sceKernelDelayThread(100 * 1000);
            }
            rc = scePromoterUtilityGetResult(&result);
            if (rc >= 0) rc = result;
        }
        ok = rc >= 0;
        scePromoterUtilityExit();
    }
    sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (!ok) { char text[48]; std::snprintf(text, sizeof(text), "promoter: 0x%08X", rc); detail = text; }
    return ok;
}
#else
bool Updater::stageHead(const std::string&, std::string&) { return true; }
bool Updater::promote(const std::string&, std::string& detail) {
    detail = "installing is only possible on the console";
    return false;
}
#endif
