#include "test_language.h"
#include "qr_scanner.h"
#include "cpu_scheduler.h"
#include <psp2/camera.h>
#include <vita2d.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

// Real scanner + real quirc; replace only the device/GPU API.
static unsigned char memory[1024 * 1024];
static bool allocated = false, opened = false, streaming = false;
static int fail_at = 0, textures = 0, stops = 0, closes = 0, frees = 0;
static std::atomic<int> reads{0};
static std::vector<unsigned char> frame(640 * 480, 255);
struct vita2d_texture { std::vector<unsigned char> pixels = std::vector<unsigned char>(320 * 240 * 4); };
namespace cpu_scheduler {
Thread::Thread(Role) : id_(0) {} Thread::~Thread() {}
Media::Media(Role r) : role_(r) {} Media::~Media() {}
}
int sceKernelAllocMemBlock(const char*, int, size_t size, void*) {
    assert(size == sizeof(memory) && !allocated);
    if (fail_at == 1) return -1;
    allocated = true; return 1;
}
int sceKernelGetMemBlockBase(int, void** p) { if (fail_at == 2) return -2; *p = memory; return 0; }
int sceKernelFreeMemBlock(int) { assert(allocated && !opened && !streaming); allocated = false; ++frees; return 0; }
int sceCameraOpen(int device, SceCameraInfo* info) {
    assert(device == SCE_CAMERA_DEVICE_BACK && allocated && !opened);
    assert(info->format == SCE_CAMERA_FORMAT_YUV420_PLANE && info->resolution == SCE_CAMERA_RESOLUTION_640_480);
    assert(info->sizeIBase == 640 * 480 && info->pUBase == memory + 640 * 480);
    if (fail_at == 3) return -3;
    opened = true; return 0;
}
int sceCameraStart(int) { assert(opened); if (fail_at == 4) return -4; streaming = true; return 0; }
int sceCameraStop(int) { assert(streaming); streaming = false; ++stops; return 0; }
int sceCameraClose(int) { assert(opened && !streaming); opened = false; ++closes; return 0; }
int sceCameraRead(int, SceCameraRead* read) {
    assert(streaming && read->mode == 1);
    std::memcpy(memory, frame.data(), frame.size()); read->frame = ++reads; return 0;
}
int sceKernelDelayThread(unsigned) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); return 0; }
void vita2d_wait_rendering_done() {}
vita2d_texture* vita2d_create_empty_texture(unsigned w, unsigned h) {
    assert(w == 320 && h == 240); if (fail_at == 5) return nullptr;
    ++textures; return new vita2d_texture;
}
void* vita2d_texture_get_datap(vita2d_texture* t) { return t->pixels.data(); }
unsigned vita2d_texture_get_stride(vita2d_texture*) { return 320 * 4; }
void vita2d_free_texture(vita2d_texture* t) { assert(textures > 0); --textures; delete t; }
void clean() { assert(!allocated && !opened && !streaming && textures == 0); }
int main(int argc, char** argv) {
    use_test_language();
    assert(argc == 2 || argc == 3);
    std::ifstream fixture(argv[1]); assert(fixture);
    std::string expected, line; std::getline(fixture, expected);
    std::vector<std::string> matrix;
    while (std::getline(fixture, line)) matrix.push_back(line);
    const int size = matrix.size(), scale = 8, x0 = (640-size*scale)/2, y0 = (480-size*scale)/2;
    for (int y=0; y<size; ++y) for (int x=0; x<size; ++x)
        for (int dy=0; dy<scale; ++dy) for (int dx=0; dx<scale; ++dx)
            frame[(y0+y*scale+dy)*640+x0+x*scale+dx] = matrix[y][x] == '1' ? 0 : 255;
    QrScanner scanner;
    if (argc == 3) {
        assert(scanner.start());
        std::string initial = scanner.status();
        for (int i=0; i<500 && scanner.status() == initial; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        assert(scanner.status() != initial && scanner.takeLink().empty());
        scanner.stop(); clean();
        std::cout << "Non-proxy / invalid proxy QR rejected: PASS\n";
        return 0;
    }
    for (int failure=1; failure<=5; ++failure) {
        fail_at = failure;
        assert(!scanner.start() && !scanner.active() && !scanner.status().empty());
        clean();
    }
    fail_at = 0;
    // Normal and mirrored fixtures exercise extraction, decoding and import parsing.
    for (int pass=0; pass<2; ++pass) {
        if (pass) for (int y=0; y<480; ++y) for (int x=0; x<320; ++x)
            std::swap(frame[y*640+x], frame[y*640+639-x]);
        assert(scanner.start() && scanner.active());
        std::string link;
        for (int i=0; i<500 && link.empty(); ++i) {
            scanner.upload(); link = scanner.takeLink();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        assert(link == expected && scanner.takeLink().empty());
        scanner.stop(); clean(); assert(!scanner.active());
        int previous_stops = stops; scanner.stop(); assert(stops == previous_stops);
    }
    // Cancel on a blank frame; restart is possible and no stale result survives.
    std::fill(frame.begin(), frame.end(), 255);
    assert(scanner.start()); std::this_thread::sleep_for(std::chrono::milliseconds(25));
    assert(scanner.takeLink().empty()); scanner.stop(); clean();
    assert(scanner.start()); scanner.stop(); clean();
    assert(reads > 0 && closes > 0 && frees > 0);
    std::cout << "QR decode, mirror, cancellation, restart and camera failure cleanup: PASS\n";
}
