#include "module_api.h"
#include "webapp_view.h"
#include "diag_log.h"
#include "memory_hygiene.h"
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <pthread.h>
#include <vita2d.h>

extern "C" void _init_vita_heap();
extern "C" void _init_vita_reent();
extern "C" void _init_vita_malloc();
extern "C" void _init_vita_io();
extern "C" void _free_vita_io();
extern "C" void _free_vita_malloc();
extern "C" void _free_vita_reent();
extern "C" void _free_vita_heap();
extern "C" void _free_vita_newlib() { _free_vita_io(); _free_vita_malloc(); _free_vita_reent(); _free_vita_heap(); }
extern "C" void __libc_init_array();
extern "C" void _init() {}
extern "C" void _fini() {}
// shortcut: isolated runtime reserves 96 MiB; tune after physical Vita memory measurements.
extern "C" { unsigned int _newlib_heap_size_user = 96 * 1024 * 1024; }

namespace {
WebKitHost host{};
WebAppView* view = nullptr;
std::string events, link;
bool started = false;
}

// Texture ownership stays with the host's initialized vita2d/GXM context.
extern "C" vita2d_texture* vita2d_create_empty_texture_format(unsigned w, unsigned h, SceGxmTextureFormat format) { return host.texture_create(w, h, format); }
extern "C" void vita2d_free_texture(vita2d_texture* texture) { host.texture_free(texture); }
extern "C" void* vita2d_texture_get_datap(const vita2d_texture* texture) { return host.texture_data(texture); }
extern "C" unsigned vita2d_texture_get_stride(const vita2d_texture* texture) { return host.texture_stride(texture); }
extern "C" void vitagram_module_diagnostic(unsigned stage, int64_t code) { if (host.diagnostic) host.diagnostic(stage, code); }

extern "C" int module_start(SceSize size, void* data) {
    if (started || !data || size != sizeof(WebKitStart*)) return SCE_KERNEL_START_FAILED;
    auto* start = *static_cast<WebKitStart**>(data);
    if (!start || start->abi != VITAGRAM_WEBKIT_ABI || start->size != sizeof(WebKitStart) ||
        start->host.abi != VITAGRAM_WEBKIT_ABI || start->host.size != sizeof(WebKitHost) ||
        !start->host.texture_create || !start->host.texture_free || !start->host.texture_data ||
        !start->host.texture_stride || !start->host.diagnostic) return SCE_KERNEL_START_FAILED;
    SceKernelFreeMemorySizeInfo memory{}; memory.size = sizeof(memory);
    if (sceKernelGetFreeMemorySize(&memory) < 0 || memory.size_user < int(_newlib_heap_size_user + (4u << 20)))
        return SCE_KERNEL_START_FAILED;
    host = start->host;
    _init_vita_heap(); _init_vita_reent(); _init_vita_malloc(); _init_vita_io();
    pthread_init();
    __libc_init_array();
    view = new WebAppView;
    started = true;
    start->api = {
        VITAGRAM_WEBKIT_ABI, sizeof(WebKitApi),
        [](const char* url, int same, int kind, const char* server, int port, const char* user, const char* password, int enabled) {
            ProxyConfig proxy; proxy.kind = proxy_kind_from_int(kind); proxy.server = server;
            proxy.port = port; proxy.username = user; proxy.password = password;
            return int(view->open(url, same, enabled, proxy));
        },
        [] { view->close(); }, [] { view->shutdown(); }, [] { return int(view->active()); },
        [](unsigned mask, int wipe) { diag_log::set_mask(mask); memory_hygiene::set_enabled(wipe); view->pump(); },
        [](int x, int y) { view->move(x, y); }, [](int x, int y, int down) { view->click(x, y, down); },
        [](int dy, int dx) { view->scroll(dy, dx); }, [](int rotation, int full) { view->orientation(rotation, full); },
        [] { return view->rotation(); }, [] { return int(view->fullscreen()); },
        [](float scale) { view->fontScale(scale); }, [](const char* js) { view->evaluate(js); },
        [](const char* type, const char* json) { view->event(type, json); },
        [] { events = view->takeEvents(); return events.c_str(); },
        [] { link = view->takeLink(); return link.c_str(); },
        [] { return view->error().c_str(); }, [] { return int(view->loading()); }, [] { return view->texture(); }
    };
    return SCE_KERNEL_START_SUCCESS;
}

// WebKit owns live singleton threads until process exit, so unloading is forbidden.
extern "C" int module_stop(SceSize, void*) { return started ? SCE_KERNEL_STOP_CANCEL : SCE_KERNEL_STOP_SUCCESS; }
