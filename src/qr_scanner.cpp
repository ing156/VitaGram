#include "qr_scanner.h"
#include "proxy_link.h"
#include "cpu_scheduler.h"
#include <quirc.h>
#include <vita2d.h>
#include <psp2/camera.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <cstdio>
#include <cstring>

namespace {
constexpr int Width = 640, Height = 480, PreviewWidth = 320, PreviewHeight = 240;
constexpr int Device = SCE_CAMERA_DEVICE_BACK;
}
QrScanner::QrScanner() { pthread_mutex_init(&mutex_, nullptr); }
QrScanner::~QrScanner() { stop(); pthread_mutex_destroy(&mutex_); }

bool QrScanner::fail(const char* operation, int code) {
    char message[160];
    std::snprintf(message, sizeof(message), "%s: 0x%08X", operation, (unsigned)code);
    stop();
    pthread_mutex_lock(&mutex_); status_ = message; pthread_mutex_unlock(&mutex_);
    return false;
}

bool QrScanner::start() {
    stop();
    stopping_ = false;
    link_.clear(); preview_ready_ = false;
    status_ = "Наведите заднюю камеру на QR-код MTProxy";
    decoder_ = quirc_new();
    if (!decoder_ || quirc_resize(decoder_, Width, Height) < 0)
        return fail("Недостаточно памяти для QR", -1);
    // Camera DMA needs physically contiguous, uncached memory, aligned to 1 MiB.
    memory_ = sceKernelAllocMemBlock("VitaGramCamera", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
                                     1024 * 1024, nullptr);
    if (memory_ < 0) return fail("Память камеры", memory_);
    void* buffer = nullptr;
    int rc = sceKernelGetMemBlockBase(memory_, &buffer);
    if (rc < 0) return fail("Буфер камеры", rc);
    camera_buffer_ = static_cast<unsigned char*>(buffer);
    SceCameraInfo info{};
    info.size = sizeof(info);
    info.priority = SCE_CAMERA_PRIORITY_SHARE;
    info.format = SCE_CAMERA_FORMAT_YUV420_PLANE;
    info.resolution = SCE_CAMERA_RESOLUTION_640_480;
    info.framerate = SCE_CAMERA_FRAMERATE_15_FPS;
    info.range = 1;
    info.sizeIBase = Width * Height;
    info.sizeUBase = info.sizeVBase = Width * Height / 4;
    info.pIBase = buffer;
    info.pUBase = camera_buffer_ + info.sizeIBase;
    info.pVBase = camera_buffer_ + info.sizeIBase + info.sizeUBase;
    rc = sceCameraOpen(Device, &info);
    if (rc < 0) return fail("Не удалось открыть заднюю камеру", rc);
    camera_open_ = true;
    rc = sceCameraStart(Device);
    if (rc < 0) return fail("Не удалось включить камеру", rc);
    camera_started_ = true;
    vita2d_wait_rendering_done();
    texture_ = vita2d_create_empty_texture(PreviewWidth, PreviewHeight);
    if (!texture_) return fail("Недостаточно памяти для превью", -1);
    std::memset(vita2d_texture_get_datap(texture_), 0, vita2d_texture_get_stride(texture_) * PreviewHeight);
    preview_pixels_.resize(PreviewWidth * PreviewHeight);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    rc = pthread_create(&thread_, &attr, entry, this);
    pthread_attr_destroy(&attr);
    if (rc) return fail("Не удалось запустить QR-сканер", rc);
    thread_started_ = started_ = true;
    return true;
}

void QrScanner::stop() {
    stopping_ = true;
    if (thread_started_) { pthread_join(thread_, nullptr); thread_started_ = false; }
    if (camera_started_) { sceCameraStop(Device); camera_started_ = false; }
    if (camera_open_) { sceCameraClose(Device); camera_open_ = false; }
    if (memory_ >= 0) { sceKernelFreeMemBlock(memory_); memory_ = -1; }
    camera_buffer_ = nullptr;
    if (decoder_) { quirc_destroy(decoder_); decoder_ = nullptr; }
    if (texture_) { vita2d_wait_rendering_done(); vita2d_free_texture(texture_); texture_ = nullptr; }
    std::vector<uint32_t>().swap(preview_pixels_);
    preview_ready_ = false; started_ = false;
}

void* QrScanner::entry(void* self) { static_cast<QrScanner*>(self)->run(); return nullptr; }
void QrScanner::run() {
    cpu_scheduler::Thread role(cpu_scheduler::Role::Video);
    cpu_scheduler::Media workload(cpu_scheduler::Role::Video);
    uint64_t last_frame = UINT64_MAX;
    while (!stopping_) {
        SceCameraRead read{};
        read.size = sizeof(read); read.mode = 1; // Nonblocking latest-frame read.
        int rc = sceCameraRead(Device, &read);
        if (rc >= 0 && read.frame != last_frame) {
            last_frame = read.frame;
            uint8_t* gray = quirc_begin(decoder_, nullptr, nullptr);
            std::memcpy(gray, camera_buffer_, Width * Height);
            pthread_mutex_lock(&mutex_);
            for (int y = 0; y < PreviewHeight; ++y)
                for (int x = 0; x < PreviewWidth; ++x) {
                    uint32_t v = gray[(y * 2) * Width + x * 2];
                    preview_pixels_[y * PreviewWidth + x] = 0xff000000u | v | (v << 8) | (v << 16);
                }
            preview_ready_ = true;
            pthread_mutex_unlock(&mutex_);
            quirc_end(decoder_);
            for (int i = 0; i < quirc_count(decoder_); ++i) {
                quirc_code code{}; quirc_data data{};
                quirc_extract(decoder_, i, &code);
                auto error = quirc_decode(&code, &data);
                if (error == QUIRC_ERROR_DATA_ECC) { quirc_flip(&code); error = quirc_decode(&code, &data); }
                if (error != QUIRC_SUCCESS) continue;
                std::string payload(reinterpret_cast<char*>(data.payload), data.payload_len);
                ProxyLink proxy; std::string message;
                auto parsed = parse_proxy_link(payload, proxy, message);
                pthread_mutex_lock(&mutex_);
                if (parsed == ProxyLinkResult::Valid) link_ = payload;
                else status_ = parsed == ProxyLinkResult::Invalid ? message : "В QR-коде нет ссылки MTProxy";
                pthread_mutex_unlock(&mutex_);
                if (parsed == ProxyLinkResult::Valid) return;
            }
        } else if (rc < 0 && rc != (int)SCE_CAMERA_ERROR_ALREADY_READ && rc != (int)SCE_CAMERA_ERROR_NOT_ACTIVE &&
                   rc != (int)SCE_CAMERA_ERROR_TIMEOUT) {
            char message[120];
            std::snprintf(message, sizeof(message), "Ошибка чтения камеры: 0x%08X", (unsigned)rc);
            pthread_mutex_lock(&mutex_); status_ = message; pthread_mutex_unlock(&mutex_);
        }
        // Decode at most five VGA frames per second, leaving UI/TDLib responsive.
        for (int i = 0; i < 20 && !stopping_; ++i) sceKernelDelayThread(10000);
    }
}

void QrScanner::upload() {
    if (!texture_) return;
    vita2d_wait_rendering_done();
    pthread_mutex_lock(&mutex_);
    if (preview_ready_) {
        auto* dst = static_cast<unsigned char*>(vita2d_texture_get_datap(texture_));
        unsigned stride = vita2d_texture_get_stride(texture_);
        for (int y = 0; y < PreviewHeight; ++y)
            std::memcpy(dst + y * stride, preview_pixels_.data() + y * PreviewWidth, PreviewWidth * 4);
        preview_ready_ = false;
    }
    pthread_mutex_unlock(&mutex_);
}
std::string QrScanner::takeLink() {
    pthread_mutex_lock(&mutex_); std::string result; result.swap(link_); pthread_mutex_unlock(&mutex_);
    return result;
}
std::string QrScanner::status() {
    pthread_mutex_lock(&mutex_); std::string result = status_; pthread_mutex_unlock(&mutex_);
    return result;
}
