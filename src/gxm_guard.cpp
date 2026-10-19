#include <psp2/gxm.h>
#include <cstdio>

namespace {
// Rendering uses one GXM context on the UI thread, including vita2d's static library.
bool failed_scene = false;
int last_error = 0;

int failed(const char* operation, int error) {
    failed_scene = true;
    if (error != last_error) {
        std::fprintf(stderr, "GXM: %s failed (0x%08X); skipping scene\n", operation, (unsigned)error);
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
    return __real_sceGxmDraw(context, primitive, format, indices, count);
}
}
