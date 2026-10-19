#include "../src/gxm_guard.cpp"
#include <initializer_list>
#include <cassert>
#include <cstdio>

namespace {
int begin_result = 0, reserve_result = 0, uniform_result = 0;
bool write_buffer = true;
int reservations = 0, writes = 0, draws = 0;
float storage[16]{};

void begin() {
    assert(__wrap_sceGxmBeginScene(nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == begin_result);
}
void draw(bool accepted) {
    const int before = draws;
    assert((__wrap_sceGxmDraw(nullptr, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, nullptr, 3) == 0) == accepted);
    assert(draws == before + (accepted ? 1 : 0));
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
    return 0;
}
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
    std::puts("GXM guard: failed reservations, invalid outputs, scene failure and recovery PASS");
}
