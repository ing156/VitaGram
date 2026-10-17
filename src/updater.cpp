#include "updater.h"
#include "https_client.h"
#include "update_core.h"
#include "package_promote.h"
#include "vpk_package.h"
#include <cstdio>
#include <vector>
#ifdef __vita__
#include <psp2/appmgr.h>
#include <psp2/io/devctl.h>
#include <sys/stat.h>
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
// The promoter wants a shallow folder (deep ones failed on the console, shallow ones are what VitaShell and
// PSVitaAlive use), so the unpacked package does not live inside the update folder.
const char kPackageDir[] = "ux0:data/vitagram-pkg";
const char kUpdaterPackageDir[] = "ux0:data/vitagram-updater-pkg";
// The VPK and its unpacked copy exist side by side, plus the signature, marker and head.bin.
constexpr uint64_t kSpaceMargin = 16ull << 20;
constexpr uint64_t kUnknownSize = 64ull << 20; // release without a reported size

#ifdef __vita__
struct DevInfo { int64_t max_size, free_size, cluster_size; void* unknown; }; // layout of devctl 0x3001
#endif
// Free bytes on the device holding `path`; false when it cannot be determined (then nothing is refused).
bool free_space(const std::string& path, uint64_t& bytes) {
#ifdef __vita__
    size_t colon = path.find(':');
    if (colon == std::string::npos) return false;
    DevInfo info{};
    if (sceIoDevctl(path.substr(0, colon + 1).c_str(), 0x3001, nullptr, 0, &info, sizeof(info)) < 0 || info.free_size < 0) return false;
    bytes = (uint64_t)info.free_size;
    return true;
#else
    (void)path; (void)bytes;
    return false;
#endif
}

// A directory is usable when it can be created and a file written into it.
bool writable_dir(const std::string& dir, std::string& why) {
    if (!update::make_directories(dir)) { why = "cannot create " + dir; return false; }
    const std::string probe = dir + "/probe";
    FILE* f = std::fopen(probe.c_str(), "wb");
    bool ok = f && std::fputc('0', f) != EOF;
    if (f && std::fclose(f) != 0) ok = false;
    std::remove(probe.c_str());
    if (!ok) why = "cannot write in " + dir;
    return ok;
}

std::string mib(uint64_t bytes) { return std::to_string((bytes + (1u << 20) - 1) >> 20) + " MiB"; }
}

Updater::Updater(std::string current_version, std::string ca_file, std::string work_dir, std::string fallback_dir)
    : current_(std::move(current_version)), ca_file_(std::move(ca_file)), work_dir_(std::move(work_dir)),
      fallback_dir_(std::move(fallback_dir)) {}

// Picks the first of work_dir_/fallback_dir_ that is writable and has room for the package and its
// unpacked copy. A stale staged package in the other directory is dropped so that only one can be applied.
bool Updater::chooseDir(uint64_t package_size, std::string& dir, std::string& detail) {
    const uint64_t need = (package_size ? package_size : kUnknownSize) * 2 + kSpaceMargin;
    std::vector<std::string> candidates{work_dir_};
    if (!fallback_dir_.empty() && fallback_dir_ != work_dir_) candidates.push_back(fallback_dir_);
    detail.clear();
    for (const std::string& candidate : candidates) {
        std::string why;
        uint64_t free_bytes = 0;
        if (!writable_dir(candidate, why)) { detail += (detail.empty() ? "" : "; ") + why; continue; }
        if (free_space(candidate, free_bytes) && free_bytes < need) {
            detail += (detail.empty() ? "" : "; ") + std::string("not enough space on ") + candidate + ": need " + mib(need) +
                      ", free " + mib(free_bytes);
            continue;
        }
        dir = candidate;
        for (const std::string& other : candidates)
            if (other != dir) std::remove((other + "/pending").c_str());
        return true;
    }
    return false;
}

Updater::~Updater() {
    cancel_ = true;
    finishThread();
}

Updater::Snapshot Updater::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}

bool Updater::allowsLocalInstall() const {
    const Snapshot state = snapshot();
    return !state.busy() && state.state != State::Installed && !hasPending(work_dir_, fallback_dir_);
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
    std::string dir, why;
    if (!chooseDir(expected, dir, why)) { fail(Error::Storage, why); return; }
    const std::string vpk_path = dir + "/update.vpk", package_dir = dir + "/pkg";
    update::remove_tree(package_dir);

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
    // Stage only: applyPending() installs at the next start and verifies the signature again, because
    // the card is writable by other software in between. The marker is written last.
    FILE* sig_file = std::fopen((dir + "/update.sig").c_str(), "wb");
    bool sig_ok = sig_file && std::fwrite(signature.data(), 1, signature.size(), sig_file) == signature.size();
    if (sig_file && std::fclose(sig_file) != 0) sig_ok = false;
    if (!sig_ok) { fail(Error::Storage, "cannot write the signature"); return; }
    FILE* marker = std::fopen((dir + "/pending").c_str(), "wb");
    bool marker_ok = marker && std::fwrite(version.data(), 1, version.size(), marker) == version.size();
    if (marker && std::fclose(marker) != 0) marker_ok = false;
    if (!marker_ok) { fail(Error::Storage, "cannot write the install marker"); return; }
    update::remove_tree(package_dir); // only a trial unpack: the next start unpacks the verified VPK again
    set(State::Installed); // the verified VPK stays until applyPending() succeeds, for a manual install
}

namespace {
bool read_small(const std::string& path, size_t limit, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buffer[256];
    size_t n;
    out.clear();
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        out.append(buffer, n);
        if (out.size() > limit) { std::fclose(f); return false; }
    }
    std::fclose(f);
    return true;
}

// Re-checks the stored package against the release signature; the version in the marker is part of
// the signed message, so a tampered marker or package fails here.
// Checks the open package against the release signature and leaves it rewound: the caller unpacks
// exactly the file that was checked (replacing the name in between has no effect).
bool verify_staged(FILE* vpk, const std::string& work_dir, const std::string& version, std::string& detail,
                   const std::function<void(int)>& progress) {
    std::string signature;
    if (version.empty() || !read_small(work_dir + "/update.sig", kMaxSignature, signature) || signature.empty()) {
        detail = "the staged signature is missing"; return false;
    }
    update::SignatureVerifier verifier;
    if (!verifier.begin(version)) { detail = "cannot load the signing key"; return false; }
    std::vector<unsigned char> buffer(64 * 1024);
    size_t n;
    std::fseek(vpk, 0, SEEK_END);
    const long total = std::ftell(vpk);
    std::rewind(vpk);
    long done = 0;
    while ((n = std::fread(buffer.data(), 1, buffer.size(), vpk)) > 0) {
        verifier.update(buffer.data(), n);
        done += (long)n;
        if (progress && total > 0) progress((int)(done * 100 / total));
    }
    if (std::ferror(vpk) || !verifier.finish(reinterpret_cast<const unsigned char*>(signature.data()), signature.size())) {
        detail = "the staged package does not match its signature"; return false;
    }
    std::rewind(vpk);
    return true;
}
}

#ifdef __vita__
namespace {
const char kUpdaterApp[] = "ux0:app/VGUPDATE1";
const char kUpdaterTitle[] = "VGUPDATE1";
const char kUpdaterVpk[] = "app0:updater/vitagram-updater.vpk";
const char kUpdaterHead[] = "app0:updater/head.bin";
const char kHandoffDir[] = "ux0:data/vitagram/update";
const char kUpdateFallbackDir[] = "ux0:downloads/vitagram-update";
const char kHandoffFile[] = "ux0:data/vitagram/update/handoff"; // where the updater app looks for it

bool copy_file(const char* from, const std::string& to) {
    FILE* in = std::fopen(from, "rb");
    if (!in) return false;
    FILE* out = std::fopen(to.c_str(), "wb");
    if (!out) { std::fclose(in); return false; }
    char buffer[16 * 1024];
    size_t n;
    bool ok = true;
    while ((n = std::fread(buffer, 1, sizeof(buffer), in)) > 0) if (std::fwrite(buffer, 1, n, out) != n) ok = false;
    std::fclose(in);
    return std::fclose(out) == 0 && ok;
}
}

bool Updater::handoffConfirmedPackage(const std::string& package_dir, const HandoffText& text, std::string& detail) {
    if (hasPending(kHandoffDir, kUpdateFallbackDir)) {
        detail = "a signed update is already waiting for installation"; return false;
    }
    std::string bytes;
    if (!read_small(package_dir + "/sce_sys/param.sfo", 1024 * 1024, bytes)) {
        detail = "cannot read local VitaGram package metadata"; return false;
    }
    vpk::Metadata metadata;
    if (!vpk::parse_sfo(std::vector<uint8_t>(bytes.begin(), bytes.end()), metadata) || metadata.title_id != "VITAGRM01") {
        detail = "local package is not VitaGram"; return false;
    }
    if (!startInstaller(kHandoffDir, package_dir, text, detail)) return false;
    return true;
}

// Hands the unpacked package to the VitaGram Updater app. The running application cannot be replaced,
// so the updater app (a separate title) closes VitaGram and installs the package after this process ends.
bool Updater::prepareInstaller(const std::string& work_dir, std::string& detail) {
    update::make_directories(work_dir);
    struct stat info;
    const std::string updater_eboot = std::string(kUpdaterApp) + "/eboot.bin";
    if (stat(updater_eboot.c_str(), &info) != 0) {
        // First update: install the updater app packaged inside this VPK. It is a different title, so the
        // console accepts it while VitaGram runs.
        const std::string package = kUpdaterPackageDir;
        update::remove_tree(package);
        std::string reason, error;
        bool installed = update::extract_zip(kUpdaterVpk, package, nullptr, error);
        if (!installed) reason = "package: " + error;
        // head.bin carries the content id of its own title: the updater app's one comes from its package
        // (assets/updater-head.bin), VitaGram's own would not match this title id.
        struct stat head_info;
        const bool own_head = stat(kUpdaterHead, &head_info) == 0;
        if (installed) installed = stageHead(package, reason, own_head ? kUpdaterHead : "app0:sce_sys/package/head.bin");
        if (installed && !own_head) {
            // Retarget the installed homebrew header to the helper's own ID.
            std::string bytes;
            const std::string path = package + "/sce_sys/package/head.bin";
            installed = read_small(path, 1024 * 1024, bytes);
            std::vector<uint8_t> head(bytes.begin(), bytes.end());
            vpk::Metadata helper; helper.title_id = kUpdaterTitle;
            if (installed) installed = vpk::make_head(head, helper);
            FILE* f = installed ? std::fopen(path.c_str(), "wb") : nullptr;
            installed = f && std::fwrite(head.data(), 1, head.size(), f) == head.size();
            if (f && std::fclose(f) != 0) installed = false;
            if (!installed) reason = "cannot prepare updater package header";
        }
        struct stat package_eboot;
        const long long expected = stat((package + "/eboot.bin").c_str(), &package_eboot) == 0 ? (long long)package_eboot.st_size : -1;
        if (installed) installed = promote_package(package, reason, updater_eboot.c_str(), expected);
        if (installed) {
            // The promoter has reported success with nothing installed (result.txt showed an empty reason):
            // give the files a few seconds to appear, then say so instead of failing silently.
            for (int waited = 0; waited < 50 && stat(updater_eboot.c_str(), &info) != 0; ++waited) sceKernelDelayThread(100 * 1000);
            if (stat(updater_eboot.c_str(), &info) != 0) {
                installed = false;
                reason = "promoter reported success but " + updater_eboot + " is missing" + (reason.empty() ? "" : " (" + reason + ")");
            }
        }
        update::remove_tree(package);
        if (!installed || stat(updater_eboot.c_str(), &info) != 0) {
            if (reason.empty()) reason = updater_eboot + " is missing";
            // Leave the updater's own VPK next to the update so it can be installed from VitaShell.
            const std::string target = work_dir + "/vitagram-updater.vpk";
            const bool copied = copy_file(kUpdaterVpk, target);
            detail = "helper: the VitaGram Updater app could not be installed automatically (" + reason +
                     "); install " + (copied ? target : std::string(kUpdaterVpk)) + " with VitaShell";
            return false;
        }
    }
    return true;
}

bool Updater::prepareLocalInstaller(std::string& detail) {
    if (hasPending(kHandoffDir, kUpdateFallbackDir)) {
        detail = "a signed update is already waiting for installation"; return false;
    }
    return prepareInstaller(kHandoffDir, detail);
}

bool Updater::startInstaller(const std::string& work_dir, const std::string& package_dir, const HandoffText& text, std::string& detail) {
    if (!prepareInstaller(work_dir, detail)) return false;
    update::make_directories(kHandoffDir);
    FILE* f = std::fopen(kHandoffFile, "wb");
    // Text lines are single lines: strip anything that would break the format.
    auto line = [](std::string value) { for (char& c : value) if (c == '\n' || c == '\r') c = ' '; return value; };
    const std::string body = work_dir + "\n" + package_dir + "\n" + line(text.title) + "\n" + line(text.hint) + "\n";
    bool written = f && std::fwrite(body.data(), 1, body.size(), f) == body.size();
    if (f && std::fclose(f) != 0) written = false;
    if (!written) { detail = "cannot write the hand-off file"; return false; }
    // Plain launch from the main thread, no delays or other-app shutdown first (PSVitaAlive found those hang).
    const std::string uri = std::string("psgm:play?titleid=") + kUpdaterTitle;
    int rc = sceAppMgrLaunchAppByUri(0xFFFFF, uri.c_str());
    if (rc < 0) {
        char code[64];
        std::snprintf(code, sizeof(code), "helper launch: 0x%08X", rc);
        std::remove(kHandoffFile);
        detail = code;
        return false;
    }
    return true;
}
#else
bool Updater::prepareLocalInstaller(std::string& detail) {
    detail = "installing is only possible on the console"; return false;
}
bool Updater::prepareInstaller(const std::string&, std::string& detail) {
    detail = "installing is only possible on the console"; return false;
}
bool Updater::handoffConfirmedPackage(const std::string&, const HandoffText&, std::string& detail) {
    detail = "installing is only possible on the console"; return false;
}
bool Updater::startInstaller(const std::string&, const std::string&, const HandoffText&, std::string& detail) {
    detail = "installing is only possible on the console";
    return false;
}
#endif

Updater::Pending Updater::applyPending(const std::string& work_dir, const std::string& fallback_dir,
                                       const std::string& current_version, const HandoffText& text, const Progress& progress, std::string& detail) {
    Pending result = applyIn(work_dir, current_version, text, progress, detail);
    if (result == Pending::None && !fallback_dir.empty() && fallback_dir != work_dir)
        result = applyIn(fallback_dir, current_version, text, progress, detail);
    return result;
}

Updater::Pending Updater::applyIn(const std::string& work_dir, const std::string& current_version, const HandoffText& text,
                                  const Progress& progress, std::string& detail) {
    // Overall scale: signature check 0-25, unpacking 25-90, starting the updater app 90-100.
    auto report = [&](int percent) { if (progress) progress(percent); };
    const std::string marker = work_dir + "/pending", package_dir = kPackageDir, result = work_dir + "/result.txt";
    std::string version;
    if (!read_small(marker, 64, version)) {
        FILE* found = std::fopen(marker.c_str(), "rb");
        if (!found) return Pending::None;
        std::fclose(found); // present but unreadable or oversized: treated as tampered below
        version.clear();
    }
    std::remove(marker.c_str()); // one attempt only: a failure must not loop on every start
    std::remove(result.c_str());
    // The package directory staged earlier is never trusted: it is rebuilt from the re-verified VPK.
    FILE* vpk = std::fopen((work_dir + "/update.vpk").c_str(), "rb");
    bool ok = vpk != nullptr;
    if (!ok) detail = "the staged package is missing";
    report(0);
    if (ok) ok = verify_staged(vpk, work_dir, version, detail, [&](int p) { report(p * 25 / 100); });
    // A validly signed but older release must not be installed over a newer one.
    if (ok && update::compare_versions(current_version, version) >= 0) {
        detail = "the staged package (" + version + ") is not newer than " + current_version;
        ok = false;
    }
    update::remove_tree(work_dir + "/pkg"); // left by older versions and by the download step
    update::remove_tree(package_dir);
    std::string error;
    if (ok && !update::extract_zip(vpk, package_dir, [&](uint64_t done, uint64_t total) {
            report(25 + (total ? (int)(done * 65 / total) : 0));
            return true;
        }, error)) { detail = error; ok = false; }
    if (vpk) std::fclose(vpk);
    if (ok) ok = stageHead(package_dir, detail);
    report(90);
    if (ok) ok = startInstaller(work_dir, package_dir, text, detail);
    if (!ok) {
        update::remove_tree(package_dir);
        std::remove((work_dir + "/update.sig").c_str());
        if (FILE* f = std::fopen(result.c_str(), "wb")) { std::fwrite(detail.data(), 1, detail.size(), f); std::fclose(f); }
        return Pending::Failed;
    }
    // The package, VPK and signature stay for the updater app, which removes them after installing.
    return Pending::Handoff;
}

bool Updater::updaterCleanupDue(const std::string& work_dir, const std::string& fallback_dir) {
    for (const std::string& dir : {work_dir, fallback_dir}) {
        if (dir.empty()) continue;
        if (FILE* flag = std::fopen((dir + "/cleanup").c_str(), "rb")) { std::fclose(flag); return true; }
    }
    return false;
}

#ifdef __vita__
void Updater::removeUpdaterApp(const std::string& work_dir, const std::string& fallback_dir) {
    bool flagged = false;
    for (const std::string& dir : {work_dir, fallback_dir}) {
        if (dir.empty()) continue;
        if (FILE* flag = std::fopen((dir + "/cleanup").c_str(), "rb")) { std::fclose(flag); flagged = true; }
        std::remove((dir + "/cleanup").c_str()); // one attempt: a failure must not delay every start
    }
    struct stat info;
    if (!flagged || stat((std::string(kUpdaterApp) + "/eboot.bin").c_str(), &info) != 0) return;
    // The updater app exits a moment after starting VitaGram; the promoter cannot delete a running title.
    sceAppMgrDestroyOtherApp();
    std::string detail;
    delete_package(kUpdaterTitle, detail);
}
#else
void Updater::removeUpdaterApp(const std::string&, const std::string&) {}
#endif

bool Updater::hasPending(const std::string& work_dir, const std::string& fallback_dir) {
    for (const std::string& dir : {work_dir, fallback_dir}) {
        if (dir.empty()) continue;
        if (FILE* marker = std::fopen((dir + "/pending").c_str(), "rb")) { std::fclose(marker); return true; }
    }
    return false;
}

void Updater::reportStartupFailure(const std::string& detail) {
    fail(detail.compare(0, 7, "helper:") == 0 ? Error::Helper : Error::Install, detail);
}

#ifdef __vita__
bool Updater::stageHead(const std::string& package_dir, std::string& detail, const char* source_path) {
    // The promoter wants sce_sys/package/head.bin, which only the console's own installer creates.
    // The installed copy of this very app has one for the same title id, so reuse it.
    const std::string head_dir = package_dir + "/sce_sys/package", head_path = head_dir + "/head.bin";
    FILE* source = std::fopen(source_path, "rb");
    if (!source) { detail = std::string(source_path) + " is missing"; return false; }
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

#else
bool Updater::stageHead(const std::string&, std::string&, const char*) { return true; }
#endif
