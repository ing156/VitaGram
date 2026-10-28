#include <psp2/gxm.h>
#include <psp2/display.h>
#include "startup_trace.h"
#include <cstdio>

namespace {
// Rendering uses one GXM context on the UI thread, including vita2d's static library.
bool failed_scene = false;
int last_error = 0;
unsigned logged_errors = 0;

int failed(const char* operation, int error) {
    failed_scene = true;
    if (error != last_error && logged_errors < 16) {
        ++logged_errors;
        char message[128];
        std::snprintf(message, sizeof(message), "GXM: %s failed (0x%08X); skipping scene", operation, (unsigned)error);
        startup_trace::write(message);
        last_error = error;
    }
    return error;
}

int reserve(SceGxmContext* context, void** buffer,
            int (*allocate)(SceGxmContext*, void**), const char* operation) {
    // vita2d ignores reservation errors and otherwise reuses an uninitialized stack pointer.
    if (!buffer) return failed(operation, SCE_GXM_ERROR_INVALID_POINTER);
    *buffer = nullptr;
    if (failed_scene) return SCE_GXM_ERROR_RESERVE_FAILED;
    const int result = allocate(context, buffer);
    if (result < 0 || !*buffer) {
        *buffer = nullptr;
        return failed(operation, result < 0 ? result : (int)SCE_GXM_ERROR_INVALID_POINTER);
    }
    return result;
}
}

extern "C" {
int __real_sceGxmBeginScene(SceGxmContext*, unsigned int, const SceGxmRenderTarget*,
    const SceGxmValidRegion*, SceGxmSyncObject*, SceGxmSyncObject*, const SceGxmColorSurface*,
    const SceGxmDepthStencilSurface*);
int __real_sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext*, void**);
int __real_sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext*, void**);
int __real_sceGxmSetUniformDataF(void*, const SceGxmProgramParameter*, unsigned int, unsigned int, const float*);
int __real_sceGxmDraw(SceGxmContext*, SceGxmPrimitiveType, SceGxmIndexFormat, const void*, unsigned int);
int __real_sceGxmEndScene(SceGxmContext*, const SceGxmNotification*, const SceGxmNotification*);
void __real_vita2d_swap_buffers();

int __wrap_sceGxmBeginScene(SceGxmContext* context, unsigned int flags, const SceGxmRenderTarget* target,
    const SceGxmValidRegion* region, SceGxmSyncObject* vertex_sync, SceGxmSyncObject* fragment_sync,
    const SceGxmColorSurface* color, const SceGxmDepthStencilSurface* depth) {
    const int result = __real_sceGxmBeginScene(context, flags, target, region, vertex_sync, fragment_sync, color, depth);
    failed_scene = result < 0;
    return result < 0 ? failed("BeginScene", result) : result;
}

int __wrap_sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext* context, void** buffer) {
    return reserve(context, buffer, __real_sceGxmReserveVertexDefaultUniformBuffer, "vertex uniform");
}

int __wrap_sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext* context, void** buffer) {
    return reserve(context, buffer, __real_sceGxmReserveFragmentDefaultUniformBuffer, "fragment uniform");
}

int __wrap_sceGxmSetUniformDataF(void* buffer, const SceGxmProgramParameter* parameter,
    unsigned int offset, unsigned int count, const float* source) {
    if (failed_scene) return SCE_GXM_ERROR_RESERVE_FAILED;
    if (!buffer) return failed("uniform data", SCE_GXM_ERROR_INVALID_POINTER);
    const int result = __real_sceGxmSetUniformDataF(buffer, parameter, offset, count, source);
    return result < 0 ? failed("uniform data", result) : result;
}

int __wrap_sceGxmDraw(SceGxmContext* context, SceGxmPrimitiveType primitive, SceGxmIndexFormat format,
    const void* indices, unsigned int count) {
    if (failed_scene) return SCE_GXM_ERROR_RESERVE_FAILED;
    const int result = __real_sceGxmDraw(context, primitive, format, indices, count);
    return result < 0 ? failed("Draw", result) : result;
}

int __wrap_sceGxmEndScene(SceGxmContext* context, const SceGxmNotification* vertex,
    const SceGxmNotification* fragment) {
    // Close even a partially drawn scene so the next BeginScene can recover.
    const int result = __real_sceGxmEndScene(context, vertex, fragment);
    return result < 0 ? failed("EndScene", result) : result;
}

void __wrap_vita2d_swap_buffers() {
    // Failed scenes leave old/partial contents in the back buffers; presenting them causes flicker.
    if (failed_scene) { sceDisplayWaitVblankStart(); return; }
    __real_vita2d_swap_buffers();
}
}
