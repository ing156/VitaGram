// VitaGram Updater: a tiny separate application that installs a staged VitaGram update.
// The console cannot replace an application that is running, so VitaGram verifies and unpacks the
// package, writes a hand-off file and starts this app (the scheme PSVitaAlive's self-updater uses);
// this one closes VitaGram, installs the package and starts VitaGram again.
#include "package_promote.h"
#include <dirent.h>
#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <sys/stat.h>
#include <vita2d.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {
const char kHandoff[] = "ux0:data/vitagram/update/handoff";
const char kTitleId[] = "VITAGRM01";
const char kInstalledEboot[] = "ux0:app/VITAGRM01/eboot.bin";

struct Handoff { std::string work_dir, package_dir, title, hint; };

// Lines: work folder, unpacked package folder, title, hint (title and hint already in the user's language).
bool read_handoff(Handoff& out) {
    FILE* f = std::fopen(kHandoff, "rb");
    if (!f) return false;
    char buffer[1024];
    size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
    std::fclose(f);
    buffer[n] = 0;
    std::vector<std::string> lines(1);
    for (size_t i = 0; i < n; ++i) {
        if (buffer[i] == '\n') lines.emplace_back(); else if (buffer[i] != '\r') lines.back() += buffer[i];
    }
    if (lines.size() < 4 || lines[0].empty() || lines[1].empty()) return false;
    out = {lines[0], lines[1], lines[2], lines[3]};
    return true;
}

void remove_tree(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) return;
    if (S_ISDIR(info.st_mode)) {
        if (DIR* dir = opendir(path.c_str())) {
            std::vector<std::string> names;
            while (dirent* entry = readdir(dir)) {
                std::string name = entry->d_name;
                if (name != "." && name != "..") names.push_back(name);
            }
            closedir(dir);
            for (const auto& name : names) remove_tree(path + "/" + name);
        }
        sceIoRmdir(path.c_str());
    } else {
        std::remove(path.c_str());
    }
}

// `phase` 0..1 moves a bar segment back and forth: the installer reports no progress of its own.
void draw(vita2d_pgf* font, const Handoff& text, float phase) {
    vita2d_set_clear_color(RGBA8(16,24,33,255));
    vita2d_start_drawing();
    vita2d_clear_screen();
    if (font) {
        auto centered = [&](int y, unsigned int color, float scale, const std::string& line) {
            vita2d_pgf_draw_text(font, 480 - vita2d_pgf_text_width(font, scale, line.c_str()) / 2, y, color, scale, line.c_str());
        };
        centered(225, RGBA8(35,135,210,255), 1.5f, "VitaGram");
        centered(285, RGBA8(231,238,245,255), 0.9f, text.title);
        centered(325, RGBA8(163,183,200,255), 0.65f, text.hint);
    }
    const float width = 480.0f, segment = 140.0f;
    vita2d_draw_rectangle(240, 380, width, 8, RGBA8(53,70,86,255));
    vita2d_draw_rectangle(240 + (width - segment) * phase, 380, segment, 8, RGBA8(35,135,210,255));
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void start_vitagram() {
    std::string uri = std::string("psgm:play?titleid=") + kTitleId;
    sceAppMgrLaunchAppByUri(0xFFFFF, uri.c_str());
}

// The installer blocks, so it runs on a worker thread while the main thread keeps the bar moving.
struct Job {
    std::string package_dir, detail;
    long long expected_eboot = -1;
    volatile bool installed = false, finished = false;
};

int install_thread(SceSize, void* argp) {
    Job& job = **static_cast<Job**>(argp);
    // The updater was started by VitaGram, which exits right after; give the system a moment to finish
    // closing it, and try again if the promoter still refuses.
    for (int attempt = 0; attempt < 4 && !job.installed; ++attempt) {
        sceKernelDelayThread((attempt ? 3000 : 1500) * 1000);
        job.detail.clear();
        job.installed = promote_package(job.package_dir, job.detail, kInstalledEboot, job.expected_eboot);
    }
    job.finished = true;
    return 0;
}
}

int main() {
    Handoff handoff;
    if (!read_handoff(handoff)) { start_vitagram(); sceKernelExitProcess(0); return 0; }
    vita2d_init();
    vita2d_pgf* font = vita2d_load_default_pgf();
    draw(font, handoff, 0);
    draw(font, handoff, 0);

    // PSVitaAlive's updater does the same first: make sure nothing else (VitaGram) is running.
    sceAppMgrDestroyOtherApp();

    const std::string result_path = handoff.work_dir + "/result.txt";
    std::remove(kHandoff);
    Job job;
    job.package_dir = handoff.package_dir;
    struct stat package_eboot;
    if (stat((job.package_dir + "/eboot.bin").c_str(), &package_eboot) == 0) job.expected_eboot = (long long)package_eboot.st_size;
    Job* pointer = &job;
    SceUID thread = sceKernelCreateThread("vitagram-install", install_thread, 0x10000100, 0x40000, 0, 0, nullptr);
    if (thread >= 0 && sceKernelStartThread(thread, sizeof(pointer), &pointer) >= 0) {
        for (int tick = 0; !job.finished; ++tick) {
            float t = (tick % 120) / 60.0f; // one sweep per two seconds
            draw(font, handoff, t <= 1.0f ? t : 2.0f - t);
        }
        sceKernelWaitThreadEnd(thread, nullptr, nullptr);
        sceKernelDeleteThread(thread);
    } else {
        job.installed = promote_package(job.package_dir, job.detail, kInstalledEboot, job.expected_eboot); // no thread: install without animation
    }
    if (job.installed) {
        for (const char* name : {"/update.vpk", "/update.sig", "/pending", "/result.txt"}) std::remove((handoff.work_dir + name).c_str());
        // Tell the new VitaGram that this app has done its job and can be removed again.
        if (FILE* flag = std::fopen((handoff.work_dir + "/cleanup").c_str(), "wb")) { std::fputc('1', flag); std::fclose(flag); }
    } else if (FILE* f = std::fopen(result_path.c_str(), "wb")) {
        // VitaGram shows this reason in Settings at its next start; the verified VPK stays for VitaShell.
        std::fwrite(job.detail.data(), 1, job.detail.size(), f);
        std::fclose(f);
    }
    remove_tree(job.package_dir);
    // Close right away: show the finished bar for a moment, release the screen, start VitaGram and end this
    // process (the launch hands over to the other title; PSVitaAlive's updater ends the same way).
    draw(font, handoff, job.installed ? 1.0f : 0.0f);
    sceKernelDelayThread(400 * 1000);
    vita2d_fini();
    start_vitagram();
    sceKernelExitProcess(0);
    return 0;
}
