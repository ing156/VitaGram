#include "updater.h"
#include "https_client.h"
#include "update_core.h"
#include <vita2d.h>
#include <psp2/kernel/threadmgr.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <iterator>
#include <cstdio>

namespace fs = std::filesystem;
int updater_app_main();
static std::string fixtures;
static const std::string primary = "ux0:data/vitagram/update", fallback = "ux0:downloads/vitagram-update";
static std::atomic<bool> download_entered{false}, release_download{false};
static int helper_launches = 0, app_launches = 0, promotions = 0, deletions = 0;
static bool fail_launch = false, fail_install = false, silent_install = false;
static std::string last_promoted;
static std::thread::id ui_thread;
static std::string read(const std::string& path) {
    std::ifstream file(path, std::ios::binary); assert(file);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
static void write(const std::string& path, const std::string& value) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::binary); out << value; assert(out);
}
static void copy(const std::string& from, const std::string& to) { write(to, read(from)); }
static void wait_for(const std::function<bool()>& condition) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition()) { assert(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
namespace update {
HttpsResult https_get(const std::string& url, const std::string&, const std::string&,
                     const std::function<bool(const char*, size_t)>& sink, const std::atomic<bool>* cancel,
                     const std::function<void(int64_t)>& length) {
    std::string data;
    if (url.find("/releases/latest") != std::string::npos) data = read(fixtures + "/release.json");
    else if (url == "https://fixture/vitagram.vpk.sig") data = read(fixtures + "/good.sig");
    else {
        assert(url == "https://fixture/vitagram.vpk");
        download_entered = true;
        wait_for([&] { return release_download.load() || (cancel && *cancel); });
        data = read(fixtures + "/good.vpk");
    }
    if (length) length(data.size());
    if ((cancel && *cancel) || !sink(data.data(), data.size())) return {0, -1, "cancelled"};
    return {200, (int64_t)data.size(), {}};
}
}
int sceIoDevctl(const char*, int, void*, unsigned, void*, unsigned) { return -1; }
int sceAppMgrLaunchAppByUri(int, const char* uri) {
    assert(std::this_thread::get_id() == ui_thread);
    if (std::string(uri).find("VGUPDATE1") != std::string::npos) { ++helper_launches; return fail_launch ? -1 : 0; }
    assert(std::string(uri).find("VITAGRM01") != std::string::npos); ++app_launches; return 0;
}
int sceAppMgrDestroyOtherApp() { return 0; }
bool promote_package(const std::string& package, std::string& detail, const char* installed, long long) {
    ++promotions; last_promoted = package;
    assert(fs::exists(package + "/sce_sys/package/head.bin"));
    if (fail_install) { detail = "injected promoter failure"; return false; }
    if (installed && !silent_install) copy(package + "/eboot.bin", installed);
    return true;
}
bool delete_package(const char* title, std::string&) {
    assert(std::string(title) == "VGUPDATE1"); ++deletions; fs::remove_all("ux0:app/VGUPDATE1"); return true;
}
static int (*kernel_entry)(SceSize, void*);
int sceKernelCreateThread(const char*, int (*entry)(SceSize, void*), int, unsigned, int, int, void*) { kernel_entry = entry; return 1; }
int sceKernelStartThread(SceUID, SceSize size, void* arg) { return kernel_entry(size, arg); }
int sceKernelWaitThreadEnd(SceUID, void*, void*) { return 0; }
int sceKernelDeleteThread(SceUID) { return 0; }
int sceKernelDelayThread(unsigned) { return 0; }
void sceKernelExitProcess(int) {}
int sceIoRmdir(const char* path) { return fs::remove(path) ? 0 : -1; }
int vita2d_init() { return 0; } int vita2d_fini() { return 0; }
vita2d_pgf* vita2d_load_default_pgf() { return nullptr; }
void vita2d_set_clear_color(unsigned) {} void vita2d_start_drawing() {} void vita2d_clear_screen() {}
void vita2d_end_drawing() {} void vita2d_swap_buffers() {}
void vita2d_draw_rectangle(float, float, float, float, unsigned) {}
float vita2d_pgf_text_width(vita2d_pgf*, float, const char*) { return 0; }
void vita2d_pgf_draw_text(vita2d_pgf*, float, float, unsigned, float, const char*) {}

static void stage(const std::string& dir) {
    copy(fixtures + "/good.vpk", dir + "/update.vpk");
    copy(fixtures + "/good.sig", dir + "/update.sig");
    write(dir + "/pending", "9.9.9");
}
static Updater::Snapshot download(Updater& updater) {
    download_entered = release_download = false;
    updater.check(); wait_for([&] { return !updater.snapshot().busy(); });
    assert(updater.snapshot().state == Updater::State::Available);
    updater.install(); wait_for([&] { return download_entered.load(); });
    assert(!updater.allowsLocalInstall());
    release_download = true; wait_for([&] { return !updater.snapshot().busy(); });
    return updater.snapshot();
}
int main(int argc, char** argv) {
    assert(argc == 2); fixtures = argv[1]; ui_thread = std::this_thread::get_id();
    copy(fixtures + "/head.bin", "app0:sce_sys/package/head.bin");
    copy(fixtures + "/helper.vpk", "app0:updater/vitagram-updater.vpk");
    Updater updater("0.37.73", "unused-ca", primary, fallback);
    assert(updater.allowsLocalInstall());
    assert(download(updater).state == Updater::State::Installed);
    assert(!updater.snapshot().busy() && !updater.allowsLocalInstall());
    assert(Updater::hasPending(primary, fallback));
    const std::string signed_vpk = read(primary + "/update.vpk"), signature = read(primary + "/update.sig");
    std::string detail;
    assert(!Updater::prepareLocalInstaller(detail));
    assert(!Updater::handoffConfirmedPackage("local-pkg", {"local", "hint"}, detail));
    assert(helper_launches == 0 && promotions == 0);
    assert(read(primary + "/update.vpk") == signed_vpk && read(primary + "/update.sig") == signature);
    std::vector<int> progress;
    assert(Updater::applyPending(primary, fallback, "0.37.73", {"signed", "hint"}, [&](int n) { progress.push_back(n); }, detail) == Updater::Pending::Handoff);
    assert(helper_launches == 1 && progress.back() == 90);
    assert(read(primary + "/handoff").find(primary + "\nux0:data/vitagram-pkg\nsigned\nhint\n") == 0);
    assert(updater_app_main() == 0);
    assert(app_launches == 1 && last_promoted == "ux0:data/vitagram-pkg");
    assert(read("ux0:app/VITAGRM01/eboot.bin") == std::string(256, 'E'));
    assert(!fs::exists(primary + "/pending") && !fs::exists(primary + "/update.vpk") && !fs::exists(primary + "/update.sig"));
    assert(Updater::updaterCleanupDue(primary, fallback));
    Updater::removeUpdaterApp(primary, fallback); assert(deletions == 1);
    std::puts("Signed update: check/download/signature/staging -> helper -> install/relaunch/cleanup: PASS");

    // Both in-memory state and pending markers after failed/interrupted runs block local installs.
    Updater restarted("0.37.73", "unused-ca", primary, fallback);
    for (const auto& dir : {primary, fallback}) {
        stage(dir); assert(!restarted.allowsLocalInstall());
        assert(!Updater::prepareLocalInstaller(detail));
        assert(!Updater::handoffConfirmedPackage("local-pkg", {}, detail));
        fs::remove_all(dir);
    }
    assert(restarted.allowsLocalInstall());

    // Fallback storage follows the same signed startup and helper procedure.
    write("ux0:data/blocked", "not a folder");
    Updater fallback_updater("0.37.73", "unused-ca", "ux0:data/blocked", fallback);
    assert(download(fallback_updater).state == Updater::State::Installed);
    assert(fs::exists(fallback + "/pending"));
    assert(Updater::applyPending("ux0:data/blocked", fallback, "0.37.73", {"fallback", "hint"}, {}, detail) == Updater::Pending::Handoff);
    assert(updater_app_main() == 0 && !fs::exists(fallback + "/update.vpk"));
    Updater::removeUpdaterApp(primary, fallback);

    for (int fault = 0; fault < 4; ++fault) {
        stage(primary);
        if (fault == 0) write(primary + "/update.vpk", "tampered");
        if (fault == 1) fs::remove(primary + "/update.sig");
        if (fault == 2) write(primary + "/pending", "9.9.8");
        int launches_before = helper_launches;
        assert(Updater::applyPending(primary, fallback, fault == 3 ? "9.9.9" : "0.37.73", {}, {}, detail) == Updater::Pending::Failed);
        assert(helper_launches == launches_before && !fs::exists(primary + "/pending"));
        assert(fs::exists(primary + "/update.vpk") && fs::exists(primary + "/result.txt"));
    }
    stage(primary); fail_launch = true;
    assert(Updater::applyPending(primary, fallback, "0.37.73", {}, {}, detail) == Updater::Pending::Failed);
    assert(!fs::exists(primary + "/handoff") && !fs::exists("ux0:data/vitagram-pkg"));
    fail_launch = false;
    stage(primary);
    assert(Updater::applyPending(primary, fallback, "0.37.73", {}, {}, detail) == Updater::Pending::Handoff);
    fail_install = true; assert(updater_app_main() == 0); fail_install = false;
    assert(read(primary + "/result.txt") == "injected promoter failure");
    assert(fs::exists(primary + "/update.vpk") && !fs::exists(primary + "/pending"));
    fs::remove_all(primary);
    std::puts("Fallback storage, signature tampering/missing signature, version binding, rollback, launch/install failures: PASS");

    // A promoter that reports success without installing the helper must leave a reason, not "()".
    fs::remove_all("ux0:app/VGUPDATE1"); silent_install = true;
    assert(!Updater::prepareLocalInstaller(detail));
    assert(detail.find("promoter reported success but ux0:app/VGUPDATE1/eboot.bin is missing") != std::string::npos);
    assert(detail.find("()") == std::string::npos && detail.find("vitagram-updater.vpk") != std::string::npos);
    silent_install = false;
    std::puts("Helper promoted without files: reported with a reason: PASS");

    // An explicitly confirmed local VPK still works when the signed installer is idle.
    assert(update::extract_zip(fixtures + "/good.vpk", "ux0:data/vg-vpk-install", {}, detail));
    copy(fixtures + "/head.bin", "ux0:data/vg-vpk-install/sce_sys/package/head.bin");
    assert(restarted.allowsLocalInstall() && Updater::prepareLocalInstaller(detail));
    assert(Updater::handoffConfirmedPackage("ux0:data/vg-vpk-install", {"local", "hint"}, detail));
    assert(updater_app_main() == 0 && last_promoted == "ux0:data/vg-vpk-install");
    assert(fs::exists(fixtures + "/good.vpk"));
    std::puts("Local VPK handoff: blocked by signed pending in both folders, allowed after completion: PASS");
}
