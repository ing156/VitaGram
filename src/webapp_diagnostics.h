#pragma once
#include "diag_log.h"
#include "performance.h"
#ifdef VITAGRAM_WEBKIT_MODULE
extern "C" void vitagram_module_diagnostic(unsigned, int64_t);
#endif

namespace webapp_diagnostics {
void recordUnsupported(const char* type);
enum class Stage : unsigned {
    TouchRelease, ToolbarTap, MenuOpen, InlineTap, Button, Rejected,
    Command, Link, Callback, Confirm, ConfirmDrawn, Request, Ready, Failed, Closed,
    EngineOpen, EngineInit, EngineInitialized, SurfaceReady, ContextCreate,
    ContextReady, ViewCreate, ViewReady, LoadRequest, LoadQueued,
    NavigationStart, NavigationDone, NavigationFailed, ProcessStopped,
    PaintBegin, PaintDone, ProcessLaunch, ProcessStarted, ClientReady,
    LinkResolve, LinkMain, LinkNamed, LinkOther, LinkBotReady, IpcInvalidMessage,
    EngineTrace, NavigationCommit, CurlError, HttpStatus, RenderFreeze, Proxy, Count
};
// Only fixed stage names and numeric codes may leave the WebApp path.
inline void record(Stage stage, long long code = 0) {
#ifdef VITAGRAM_WEBKIT_MODULE
    if (static_cast<unsigned>(stage) < static_cast<unsigned>(Stage::Count))
        vitagram_module_diagnostic(static_cast<unsigned>(stage), code);
#else
    static constexpr const char* names[] = {
        "touch_release", "toolbar_tap", "menu_open", "inline_tap", "button", "rejected",
        "command", "link", "callback", "confirm", "confirm_drawn", "request", "ready", "failed", "closed",
        "engine_open", "engine_init", "engine_initialized", "surface_ready", "context_create",
        "context_ready", "view_create", "view_ready", "load_request", "load_queued",
        "navigation_start", "navigation_done", "navigation_failed", "process_stopped",
        "paint_begin", "paint_done", "process_launch", "process_started", "client_ready",
        "link_resolve", "link_main", "link_named", "link_other", "link_bot_ready", "ipc_invalid_message",
        "engine_trace", "navigation_commit", "curl_error", "http_status", "render_freeze", "proxy"
    };
    static_assert(sizeof(names) / sizeof(*names) == static_cast<unsigned>(Stage::Count));
    const auto index = static_cast<unsigned>(stage);
    if (index >= static_cast<unsigned>(Stage::Count)) return;
    if (FILE* file = diag_log::open(diag_log::Kind::Performance, "ux0:data/vitagram/webapp.log", "ab")) {
        std::fprintf(file, "stage=%s code=%lld at_us=%lld session_us=%lld frame=%llu\n", names[index], code,
            (long long)performance::now(), (long long)performance::startup_origin().load(),
            (unsigned long long)performance::frame_context.frame);
        std::fclose(file);
    }
#endif
}
}
