#pragma once

#include <cstdint>
#include <memory>
#include <wpe/wpe.h>

class WPEBackendVita {
public:
    WPEBackendVita(uint32_t width, uint32_t height);
    ~WPEBackendVita();
    WPEBackendVita(const WPEBackendVita&) = delete;
    WPEBackendVita& operator=(const WPEBackendVita&) = delete;

    bool valid() const { return backend_ != nullptr; }
    wpe_view_backend* get() const { return backend_; }
    void activate();
    void pointer_move(int x, int y, uint32_t modifiers = 0);
    void pointer_button(int x, int y, bool pressed, uint32_t modifiers = 0);
    void scroll(int x, int y, double horizontal, double vertical, uint32_t modifiers = 0);
    void touch(int id, int x, int y, wpe_input_touch_event_type);
    void key(uint32_t unicode, bool pressed, uint32_t modifiers = 0);

private:
    static void* create(void*, wpe_view_backend*);
    static void destroy(void*);
    static void initialize(void*);
    static int renderer_host_fd(void*);
    static uint32_t timestamp();
    uint32_t width_;
    uint32_t height_;
    wpe_view_backend* backend_ = nullptr;
    static wpe_view_backend_interface interface_;
};
