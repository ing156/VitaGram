#include "WPEBackendVita.h"
#include <psp2/kernel/processmgr.h>
#include <algorithm>

namespace {
void* load_wpe_object(const char*) { return nullptr; }
}

extern "C" wpe_loader_interface _wpe_loader_interface = {
    load_wpe_object, nullptr, nullptr, nullptr, nullptr
};

wpe_view_backend_interface WPEBackendVita::interface_ = {
    .create = WPEBackendVita::create,
    .destroy = WPEBackendVita::destroy,
    .initialize = WPEBackendVita::initialize,
    .get_renderer_host_fd = WPEBackendVita::renderer_host_fd,
    ._wpe_reserved0 = nullptr,
    ._wpe_reserved1 = nullptr,
    ._wpe_reserved2 = nullptr,
    ._wpe_reserved3 = nullptr,
};

WPEBackendVita::WPEBackendVita(uint32_t width, uint32_t height)
    : width_(width)
    , height_(height)
    , backend_(wpe_view_backend_create_with_backend_interface(&interface_, this))
{
}

WPEBackendVita::~WPEBackendVita()
{
    if (backend_)
        wpe_view_backend_destroy(backend_);
}

void* WPEBackendVita::create(void* data, wpe_view_backend*) { return data; }
void WPEBackendVita::destroy(void*) { }
int WPEBackendVita::renderer_host_fd(void*) { return -1; }

void WPEBackendVita::initialize(void* data)
{
    auto& self = *static_cast<WPEBackendVita*>(data);
    wpe_view_backend_dispatch_set_size(self.backend_, self.width_, self.height_);
    self.activate();
}

uint32_t WPEBackendVita::timestamp()
{
    return static_cast<uint32_t>(sceKernelGetProcessTimeWide() / 1000);
}

void WPEBackendVita::activate()
{
    if (!backend_)
        return;
    wpe_view_backend_add_activity_state(backend_, wpe_view_activity_state_visible
        | wpe_view_activity_state_focused | wpe_view_activity_state_in_window);
    wpe_view_backend_set_target_refresh_rate(backend_, 60 * 1000);
}

void WPEBackendVita::pointer_move(int x, int y, uint32_t modifiers)
{
    wpe_input_pointer_event event { wpe_input_pointer_event_type_motion, timestamp(), x, y, 0, 0, modifiers };
    wpe_view_backend_dispatch_pointer_event(backend_, &event);
}

void WPEBackendVita::pointer_button(int x, int y, bool pressed, uint32_t modifiers)
{
    wpe_input_pointer_event event { wpe_input_pointer_event_type_button, timestamp(), x, y, 1,
        pressed ? 1u : 0u, modifiers | (pressed ? static_cast<uint32_t>(wpe_input_pointer_modifier_button1) : 0u) };
    wpe_view_backend_dispatch_pointer_event(backend_, &event);
}

void WPEBackendVita::scroll(int x, int y, double horizontal, double vertical, uint32_t modifiers)
{
    wpe_input_axis_2d_event event { };
    event.base = { static_cast<wpe_input_axis_event_type>(wpe_input_axis_event_type_motion_smooth
        | wpe_input_axis_event_type_mask_2d), timestamp(), x, y, 0, 0, modifiers };
    event.x_axis = horizontal;
    event.y_axis = vertical;
    wpe_view_backend_dispatch_axis_event(backend_, &event.base);
}

void WPEBackendVita::touch(int id, int x, int y, wpe_input_touch_event_type type)
{
    wpe_input_touch_event_raw point { type, timestamp(), id, x, y };
    wpe_input_touch_event event { &point, 1, type, id, point.time, 0 };
    wpe_view_backend_dispatch_touch_event(backend_, &event);
}

void WPEBackendVita::key(uint32_t unicode, bool pressed, uint32_t modifiers)
{
    auto code = wpe_unicode_to_key_code(unicode);
    wpe_input_keyboard_event event { timestamp(), code, code, pressed, modifiers };
    wpe_view_backend_dispatch_keyboard_event(backend_, &event);
}
