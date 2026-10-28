#include "webview/module_api.h"
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <vita2d.h>
#include <cassert>

extern "C" int module_start(SceSize, void*);
extern "C" int module_stop(SceSize, void*);
namespace { int initializations = 0; int free_memory = 200 << 20; vita2d_texture pixels{}; }
extern "C" void _init_vita_heap() { ++initializations; }
extern "C" void _init_vita_reent() { ++initializations; }
extern "C" void _init_vita_malloc() { ++initializations; }
extern "C" void _init_vita_io() { ++initializations; }
extern "C" void _free_vita_heap() {}
extern "C" void _free_vita_reent() {}
extern "C" void _free_vita_malloc() {}
extern "C" void _free_vita_io() {}
extern "C" void __libc_init_array() { ++initializations; }
int pthread_init() { return 1; }
namespace memory_hygiene { void set_enabled(bool) {} }
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo* memory) { memory->size_user = free_memory; return 0; }

int main() {
    WebKitStart start{};
    auto* argument = &start;
    assert(module_stop(0, nullptr) == SCE_KERNEL_STOP_SUCCESS);
    assert(module_start(sizeof(argument), &argument) == SCE_KERNEL_START_FAILED && initializations == 0);
    start.abi = VITAGRAM_WEBKIT_ABI; start.size = sizeof(start);
    start.host = {VITAGRAM_WEBKIT_ABI, sizeof(WebKitHost),
        [](unsigned, unsigned, unsigned) { return &pixels; }, [](vita2d_texture*) {},
        [](const vita2d_texture*) -> void* { return nullptr; }, [](const vita2d_texture*) -> unsigned { return 0; },
        [](unsigned, int64_t) {}};
    free_memory = 32 << 20;
    assert(module_start(sizeof(argument), &argument) == SCE_KERNEL_START_FAILED && initializations == 0);
    free_memory = 200 << 20;
    assert(module_start(sizeof(argument), &argument) == SCE_KERNEL_START_SUCCESS && initializations == 5);
    assert(webkit_api_valid(start.api));
    assert(module_start(sizeof(argument), &argument) == SCE_KERNEL_START_FAILED && initializations == 5);
    assert(module_stop(0, nullptr) == SCE_KERNEL_STOP_CANCEL);
    assert(vita2d_create_empty_texture_format(1, 1, TestFormat) == &pixels);
}
