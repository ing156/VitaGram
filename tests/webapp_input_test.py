"""Compile production Mini App input/rotation methods and native event conversion."""
from pathlib import Path
import os
import subprocess
import sys

root, build, source = map(Path, sys.argv[1:])
header = (root / 'src/webapp_view.h').read_text().split('class WebAppView {')[0].replace('#pragma once', '')
app = (root / 'src/webapp_view.cpp').read_text()
app = app[app.index('void WebAppView::move('):app.index('void WebAppView::fontScale(')]
native = (source / 'Source/WebKit/UIProcess/API/C/playstation/WKPagePrivatePlayStation.cpp').read_text()
native = native[native.index('void WKPageHandleMouseEvent('):native.index('void WKPagePaint(')]
fixture = r'''
#include <cassert>
#include <cstring>
#include <vector>
#include <WebKit/WKPagePrivatePlayStation.h>
#include <wpe/wpe.h>
#define PLATFORM(x) PLATFORM_##x
#define PLATFORM_VITA 1
#define ASSERT_NOT_REACHED() assert(false)
static uint64_t eventTime = 1234000;
static uint64_t sceKernelGetProcessTimeWide() { return eventTime; }
namespace WebKit {
struct WebWheelEvent { enum class Phase { PhaseNone }; };
struct NativeWebMouseEvent {
    wpe_input_pointer_event event;
    NativeWebMouseEvent(wpe_input_pointer_event* value, float) : event(*value) { }
};
struct NativeWebWheelEvent {
    wpe_input_axis_2d_event event;
    NativeWebWheelEvent(wpe_input_axis_event* value, float, WebWheelEvent::Phase, WebWheelEvent::Phase) {
        assert(value->type & wpe_input_axis_event_type_mask_2d);
        event = *reinterpret_cast<wpe_input_axis_2d_event*>(value);
    }
};
}
struct OpaqueWKPage {
    std::vector<wpe_input_pointer_event> mouse;
    std::vector<wpe_input_axis_2d_event> wheel;
    void handleMouseEvent(WebKit::NativeWebMouseEvent event) { mouse.push_back(event.event); }
    void handleNativeWheelEvent(WebKit::NativeWebWheelEvent event) { wheel.push_back(event.event); }
};
namespace WebKit { static OpaqueWKPage* toImpl(WKPageRef page) { return const_cast<OpaqueWKPage*>(page); } }
'''
fixture += native + header
fixture += r'''
constexpr int width = 960, height = 392;
struct Texture { int width, height; std::vector<unsigned char> data; };
static unsigned allocations = 0, frees = 0;
static bool allocationFails = false;
static Texture* vita2d_create_empty_texture_format(int w, int h, int) {
    if (allocationFails) return nullptr;
    ++allocations;
    return new Texture { w, h, std::vector<unsigned char>(w * h * 4, 99) };
}
static int vita2d_texture_get_stride(Texture* t) { return t->width * 4; }
static void* vita2d_texture_get_datap(Texture* t) { return t->data.data(); }
static void vita2d_free_texture(Texture* t) {
    for (auto byte : t->data) assert(byte == 0);
    ++frees; delete t;
}
namespace memory_hygiene { static void clear(void* p, size_t n) { std::memset(p, 0, n); } }
constexpr int SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB = 0;
static WKSize viewSize;
static void WKViewSetSize(int, WKSize size) { viewSize = size; }
struct WebAppView {
    struct Impl {
        WKPageRef page = nullptr;
        int view = 1, rotation = 0, viewport_width = width, viewport_height = height;
        int pointer_x = width / 2, pointer_y = height / 2;
        Texture* pixels = nullptr;
        bool dirty = false, fullscreen = false;
        std::string error;
    };
    std::shared_ptr<Impl> impl_ = std::make_shared<Impl>();
    bool active() const { return impl_->page; }
    void move(int, int);
    void click(int, int, bool);
    void scroll(int, int = 0);
    int rotation() const;
    bool fullscreen() const;
    void orientation(int, bool = false);
};
'''
fixture += app
main = (root / 'src/main.cpp').read_text().splitlines()
snapshot = next(line.strip() for line in main if 'const bool restore_webapp_touch =' in line)
restore = next(line.strip() for line in main if line.strip().startswith('if (restore_webapp_touch &&'))
consume = next(line.strip() for line in main if 'pressed = released = 0; tap = false; touch_blocked = true; touch.reportNum = 0;' in line)
owner = next(line.strip() for line in main if 'bool bot_touch_owned = bot_panel != 0' in line)
fixture += r'''
struct TelegramClient {
    struct BotWebApp { enum Phase { Closed, Ready }; Phase phase = Ready; } state;
    BotWebApp& botWebApp() { return state; }
};
static bool ownsTouch(TelegramClient& tg, int bot_panel) {
''' + owner + r'''
    return bot_touch_owned;
}
static bool modalFrame(TelegramClient& tg, bool& touch_blocked, bool release, bool close) {
    if (tg.botWebApp().phase == TelegramClient::BotWebApp::Closed) return false;
    bool tap = release && !touch_blocked;
    const bool delivered = tap;
    int pressed = 1, released = 1;
    struct { int reportNum = 1; } touch;
''' + snapshot + '\n' + consume + r'''
    assert(touch_blocked && !tap && !pressed && !released && !touch.reportNum);
    if (close) tg.state.phase = TelegramClient::BotWebApp::Closed;
''' + restore + r'''
    return delivered;
}
'''
fixture += r'''
int main() {
    TelegramClient tg;
    bool blocked = false;
    assert(ownsTouch(tg, 0));
    assert(!modalFrame(tg, blocked, false, false) && !blocked);
    assert(modalFrame(tg, blocked, true, false) && !blocked);
    assert(!modalFrame(tg, blocked, false, false) && !blocked);
    assert(modalFrame(tg, blocked, true, false) && !blocked);
    blocked = true;
    assert(!modalFrame(tg, blocked, true, false) && blocked);
    blocked = false;
    assert(modalFrame(tg, blocked, true, true) && blocked);
    assert(!ownsTouch(tg, 0) && ownsTouch(tg, 1));
    OpaqueWKPage page;
    WebAppView view;
    view.click(20, 30, true); view.move(20, 30); view.scroll(180); view.orientation(1);
    assert(page.mouse.empty() && page.wheel.empty() && allocations == 0);
    view.impl_->page = &page;
    view.impl_->pixels = vita2d_create_empty_texture_format(width, height, 0);
    for (bool fullscreen : {false, true, false}) for (int rotation : {0, 1, 3, 0}) {
        const int height = fullscreen ? 544 : 392;
        view.orientation(rotation, fullscreen);
        assert(view.rotation() == rotation);
        assert(view.fullscreen() == fullscreen);
        assert(view.impl_->viewport_width == (rotation ? height : 960));
        assert(view.impl_->viewport_height == (rotation ? 960 : height));
        if (rotation) assert(viewSize.width == height && viewSize.height == 960);
        for (int x = 0; x < 960; ++x) for (int y = 0; y < height; ++y) {
            auto [px, py] = webapp_page_point(rotation, x, y, 960, height);
            assert(px >= 0 && px < view.impl_->viewport_width && py >= 0 && py < view.impl_->viewport_height);
            const int sx = rotation == 1 ? 959 - py : rotation == 3 ? py : px;
            const int sy = rotation == 1 ? px : rotation == 3 ? height - 1 - px : py;
            assert(sx == x && sy == y);
        }
        page.mouse.clear(); page.wheel.clear();
        view.click(100, 50, true);
        auto [px, py] = webapp_page_point(rotation, 100, 50, 960, height);
        assert(page.mouse.size() == 2);
        auto down = page.mouse.back();
        assert(down.x == px && down.y == py && down.state == 1 && down.button == 1);
        assert(down.time == eventTime / 1000 && (down.modifiers & wpe_input_pointer_modifier_button1));
        view.click(100, 50, false);
        auto up = page.mouse.back();
        assert(up.state == 0 && up.modifiers == 0 && up.x == px && up.y == py);
        view.scroll(180); view.scroll(-180); view.scroll(0);
        assert(page.wheel.size() == 2);
        assert(page.wheel[0].x_axis == 0 && page.wheel[0].y_axis == -180);
        assert(page.wheel[1].x_axis == 0 && page.wheel[1].y_axis == 180);
        assert(page.wheel[0].base.x == px && page.wheel[0].base.y == py);
        view.scroll(0, 35);
        assert(page.wheel.back().x_axis == -35 && page.wheel.back().y_axis == 0);
        auto before = allocations;
        view.orientation(rotation, fullscreen);
        assert(allocations == before);
    }
    view.move(-30, 999);
    assert(page.mouse.back().x == 0 && page.mouse.back().y == 391);
    view.orientation(2); assert(view.rotation() == 0);
    auto* oldPixels = view.impl_->pixels;
    allocationFails = true;
    view.orientation(1);
    assert(view.rotation() == 0 && view.impl_->pixels == oldPixels && !view.impl_->error.empty());
    memory_hygiene::clear(oldPixels->data.data(), oldPixels->data.size());
    vita2d_free_texture(oldPixels);
    assert(allocations == frees);
}
'''
path = build / 'webapp_input.cpp'
path.write_text(fixture)
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I', str(root / 'src'), '-I', str(root / 'external/webkit-vita/include'),
                '-I', str(root / 'external/webkit-vita/include/wpe-1.0'), str(path),
                '-o', str(build / 'webapp_input')], check=True)
subprocess.run([str(build / 'webapp_input')], check=True)
print('Mini Apps: modal touch release/ownership, native mouse/wheel delivery, rotated hit testing and surface allocation failure passed')
