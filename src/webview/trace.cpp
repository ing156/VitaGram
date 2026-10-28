// Match only fixed upstream milestones: free-form traces can contain credentials.
#include "webapp_diagnostics.h"
#include <atomic>
#include <cstring>
#include <cstdint>

namespace {
std::atomic<uint64_t> seen { 0 };
constexpr const char* milestones[] = {
    "webkit.network.schedule.received",
    "webkit.network.serviceworker.checked",
    "webkit.network.loader.start",
    "webkit.network.request.start",
    "webkit.curl.worker.enter",
    "webkit.vita.dns.cache.miss",
    "webkit.vita.dns.cache.hit",
    "webkit.vita.dns.cache.negative",
    "webkit.vita.dns.resolved",
    "webkit.vita.dns.failed",
    "webkit.vita.dns.create.failed",
    "webkit.curl.response.dispatch",
    "webkit.loader.response.enter",
    "webkit.loader.response.metrics",
    "webkit.loader.response.sent",
    "webkit.curl.data.received",
    "webkit.curl.transfer.complete",
    "webkit.curl.transfer.failed",
    "webkit.paint.area.ready",
    "webkit.paint.area.suspended",
    "webkit.paint.schedule.first",
    "webkit.paint.display.enter",
    "webkit.paint.display.empty",
    "webkit.paint.display.suspended",
    "webkit.paint.display.frozen",
    "webkit.paint.bitmap.ready",
    "webkit.paint.bitmap.failed",
    "webkit.paint.snapshot.ready",
    "webkit.paint.snapshot.failed",
    "webkit.paint.proxy.update.enter",
    "webkit.paint.proxy.update.ready",
    "webkit.paint.proxy.damage.sent",
    "webkit.paint.backing.ready",
    "webkit.paint.backing.bitmap.failed"
};
static_assert(sizeof(milestones) / sizeof(*milestones) <= 64);
}

extern "C" void vitagram_webkit_reset_trace() { seen.store(0, std::memory_order_relaxed); }
extern "C" void vita_stream_browser_trace(const char* label) {
    if (!label || !diag_log::enabled(diag_log::Kind::Performance)) return;
    for (unsigned i = 0; i < sizeof(milestones) / sizeof(*milestones); ++i) {
        if (std::strcmp(label, milestones[i])) continue;
        const uint64_t bit = uint64_t { 1 } << i;
        if (!(seen.fetch_or(bit, std::memory_order_relaxed) & bit))
            webapp_diagnostics::record(webapp_diagnostics::Stage::EngineTrace, i + 1);
        return;
    }
}
extern "C" void vita_stream_browser_trace_value(const char* label, unsigned value) {
    if (!label) return;
    if (!std::strcmp(label, "webkit.curl.error") && value > 0 && value < 256)
        webapp_diagnostics::record(webapp_diagnostics::Stage::CurlError, value);
    else if (!std::strcmp(label, "webkit.curl.http_status") && value >= 100 && value <= 599)
        webapp_diagnostics::record(webapp_diagnostics::Stage::HttpStatus, value);
}
extern "C" void vita_stream_browser_log_message(const char*) {}
extern "C" void vita_stream_browser_log_performance(const char*, const char*) {}

namespace WebKit {
void vitagramWebKitRenderingFreeze(unsigned reasons) {
    if (reasons <= 511)
        webapp_diagnostics::record(webapp_diagnostics::Stage::RenderFreeze, reasons);
}
void vitagramWebKitInvalidMessage(unsigned message) {
    webapp_diagnostics::record(webapp_diagnostics::Stage::IpcInvalidMessage, message);
}
}

namespace webapp_diagnostics {
void recordUnsupported(const char* type) {
    if (!type || !diag_log::enabled(diag_log::Kind::Performance)) return;
    static constexpr const char* known[] = {
        "iframe_ready",
        "iframe_will_reload",
        "web_app_add_to_home_screen",
        "web_app_biometry_get_info",
        "web_app_biometry_open_settings",
        "web_app_biometry_request_access",
        "web_app_biometry_request_auth",
        "web_app_biometry_update_token",
        "web_app_check_home_screen",
        "web_app_check_location",
        "web_app_close",
        "web_app_close_scan_qr_popup",
        "web_app_data_send",
        "web_app_device_storage_clear",
        "web_app_device_storage_get_key",
        "web_app_device_storage_save_key",
        "web_app_exit_fullscreen",
        "web_app_expand",
        "web_app_hide_keyboard",
        "web_app_invoke_custom_method",
        "web_app_open_invoice",
        "web_app_open_link",
        "web_app_open_location_settings",
        "web_app_open_popup",
        "web_app_open_scan_qr_popup",
        "web_app_open_tg_link",
        "web_app_read_text_from_clipboard",
        "web_app_ready",
        "web_app_request_chat",
        "web_app_request_content_safe_area",
        "web_app_request_emoji_status_access",
        "web_app_request_file_download",
        "web_app_request_fullscreen",
        "web_app_request_location",
        "web_app_request_phone",
        "web_app_request_safe_area",
        "web_app_request_theme",
        "web_app_request_viewport",
        "web_app_request_write_access",
        "web_app_secure_storage_clear",
        "web_app_secure_storage_get_key",
        "web_app_secure_storage_restore_key",
        "web_app_secure_storage_save_key",
        "web_app_send_prepared_message",
        "web_app_set_background_color",
        "web_app_set_bottom_bar_color",
        "web_app_set_emoji_status",
        "web_app_set_header_color",
        "web_app_setup_back_button",
        "web_app_setup_closing_behavior",
        "web_app_setup_main_button",
        "web_app_setup_secondary_button",
        "web_app_setup_settings_button",
        "web_app_setup_swipe_behavior",
        "web_app_share_to_story",
        "web_app_start_accelerometer",
        "web_app_start_device_orientation",
        "web_app_start_gyroscope",
        "web_app_stop_accelerometer",
        "web_app_stop_device_orientation",
        "web_app_stop_gyroscope",
        "web_app_switch_inline_query",
        "web_app_toggle_orientation_lock",
        "web_app_trigger_haptic_feedback",
    };
    const char* label = "unknown";
    for (const auto* name : known) if (!std::strcmp(type, name)) { label = name; break; }
    if (FILE* file = diag_log::open(diag_log::Kind::Performance, "ux0:data/vitagram/webapp.log", "ab")) {
        std::fprintf(file, "stage=bridge_unsupported_%s code=0 at_us=%lld session_us=%lld frame=%llu\n", label,
            (long long)performance::now(), (long long)performance::startup_origin().load(),
            (unsigned long long)performance::frame_context.frame);
        std::fclose(file);
    }
}
}
