#include "../src/gxm_guard.cpp"
#include <initializer_list>
#include <cassert>
#include <cstdio>
#include <string>

namespace {
int begin_result = 0, reserve_result = 0, uniform_result = 0, draw_result = 0, end_result = 0;
bool write_buffer = true;
int reservations = 0, writes = 0, draws = 0;
int swaps = 0, vblanks = 0, ends = 0;
float storage[16]{};

void begin() {
    assert(__wrap_sceGxmBeginScene(nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == begin_result);
}
void draw(bool accepted) {
    const int before = draws;
    assert((__wrap_sceGxmDraw(nullptr, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, nullptr, 3) == 0) == accepted);
    assert(draws == before + (accepted ? 1 : 0));
    const int before_swaps = swaps, before_vblanks = vblanks;
    __wrap_vita2d_swap_buffers();
    assert(swaps == before_swaps + (accepted ? 1 : 0));
    assert(vblanks == before_vblanks + (accepted ? 0 : 1));
}
}

extern "C" {
int __real_sceGxmBeginScene(SceGxmContext*, unsigned int, const SceGxmRenderTarget*, const SceGxmValidRegion*,
    SceGxmSyncObject*, SceGxmSyncObject*, const SceGxmColorSurface*, const SceGxmDepthStencilSurface*) {
    return begin_result;
}
int __real_sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext*, void** buffer) {
    ++reservations;
    if (write_buffer) *buffer = storage;
    return reserve_result;
}
int __real_sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext* context, void** buffer) {
    ++reservations;
    if (write_buffer) *buffer = storage;
    (void)context;
    return reserve_result;
}
int __real_sceGxmSetUniformDataF(void* buffer, const SceGxmProgramParameter*, unsigned int, unsigned int, const float*) {
    assert(buffer == storage);
    ++writes;
    return uniform_result;
}
int __real_sceGxmDraw(SceGxmContext*, SceGxmPrimitiveType, SceGxmIndexFormat, const void*, unsigned int) {
    ++draws;
    return draw_result;
}
int __real_sceGxmEndScene(SceGxmContext*, const SceGxmNotification*, const SceGxmNotification*) { ++ends; return end_result; }
void __real_vita2d_swap_buffers() { ++swaps; }
int sceDisplayWaitVblankStart() { ++vblanks; return 0; }
}

int main() {
    using Reserve = int (*)(SceGxmContext*, void**);
    for (Reserve allocate : {__wrap_sceGxmReserveVertexDefaultUniformBuffer, __wrap_sceGxmReserveFragmentDefaultUniformBuffer}) {
        begin();
        void* buffer = nullptr;
        assert(allocate(nullptr, &buffer) == 0 && buffer == storage);
        assert(__wrap_sceGxmSetUniformDataF(buffer, nullptr, 0, 16, storage) == 0);
        draw(true);

        // Reproduce the dump: the old stack slot contains a read-only text pointer.
        for (bool writes_on_error : {false, true}) {
            begin();
            buffer = const_cast<char*>("2/5  Initializing network");
            write_buffer = writes_on_error;
            reserve_result = (int)SCE_GXM_ERROR_RESERVE_FAILED;
            assert(allocate(nullptr, &buffer) == reserve_result && buffer == nullptr);
            const int previous_writes = writes, previous_reservations = reservations;
            assert(__wrap_sceGxmSetUniformDataF(buffer, nullptr, 0, 16, storage) < 0);
            assert(allocate(nullptr, &buffer) < 0 && buffer == nullptr);
            assert(writes == previous_writes && reservations == previous_reservations);
            draw(false);
        }

        // Even a nominal success must supply a valid output pointer.
        reserve_result = 0;
        write_buffer = false;
        begin();
        assert(allocate(nullptr, &buffer) < 0 && buffer == nullptr);
        draw(false);
        write_buffer = true;
        begin();
        assert(allocate(nullptr, &buffer) == 0);
        draw(true);
    }

    begin_result = (int)SCE_GXM_ERROR_OUT_OF_MEMORY;
    begin();
    void* buffer = storage;
    const int previous_reservations = reservations;
    assert(__wrap_sceGxmReserveVertexDefaultUniformBuffer(nullptr, &buffer) < 0 && !buffer);
    assert(reservations == previous_reservations);
    draw(false);

    begin_result = 0;
    begin();
    uniform_result = (int)SCE_GXM_ERROR_INVALID_VALUE;
    assert(__wrap_sceGxmSetUniformDataF(storage, nullptr, 0, 16, storage) == uniform_result);
    draw(false);
    uniform_result = 0;
    begin();
    assert(__wrap_sceGxmSetUniformDataF(nullptr, nullptr, 0, 16, storage) < 0);
    draw(false);
    begin();
    assert(__wrap_sceGxmReserveVertexDefaultUniformBuffer(nullptr, nullptr) < 0);
    draw(false);
    begin();
    assert(__wrap_sceGxmSetUniformDataF(storage, nullptr, 0, 16, storage) == 0);
    draw(true);
    // A real draw failure also prevents presenting stale startup buffers.
    draw_result = (int)SCE_GXM_ERROR_INVALID_VALUE;
    assert(__wrap_sceGxmDraw(nullptr, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, nullptr, 3) == draw_result);
    draw(false);
    assert(__wrap_sceGxmEndScene(nullptr, nullptr, nullptr) == 0 && ends == 1);
    draw_result = 0;
    begin();
    draw(true);
    end_result = (int)SCE_GXM_ERROR_INVALID_VALUE;
    assert(__wrap_sceGxmEndScene(nullptr, nullptr, nullptr) == end_result && ends == 2);
    draw(false);
    end_result = 0;
    begin();
    draw(true);

    // Startup diagnostics obey the existing performance-log switch and reset each launch.
    const char* path = "ux0:data/vitagram/startup.log";
    std::remove(path);
    diag_log::set_mask(0);
    startup_trace::write("disabled", true);
    assert(!std::fopen(path, "rb"));
    diag_log::set_mask(1u << static_cast<unsigned>(diag_log::Kind::Performance));
    startup_trace::write("previous launch", true);
    startup_trace::write("fonts", true);
    startup_trace::write("theme");
    FILE* file = std::fopen(path, "rb"); assert(file);
    char trace[128]{};
    assert(std::fread(trace, 1, sizeof(trace) - 1, file) == 12);
    std::fclose(file);
    assert(std::string(trace) == "fonts\ntheme\n");
    std::remove(path);
    startup_trace::write("errors", true);
    logged_errors = 0;
    for (int i = 0; i < 32; ++i) {
        begin();
        uniform_result = i % 2 ? (int)SCE_GXM_ERROR_INVALID_VALUE : (int)SCE_GXM_ERROR_INVALID_POINTER;
        assert(__wrap_sceGxmSetUniformDataF(storage, nullptr, 0, 16, storage) < 0);
    }
    file = std::fopen(path, "rb"); assert(file);
    int lines = 0;
    while (std::fgets(trace, sizeof(trace), file)) ++lines;
    std::fclose(file);
    assert(lines == 17 && logged_errors == 16);
    std::remove(path);
    std::puts("GXM guard: failed reservations, invalid outputs, scene failure and recovery PASS");
}
