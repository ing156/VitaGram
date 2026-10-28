#include "webapp_diagnostics.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>
#include <regex>
#include <sstream>

extern "C" void vita_stream_browser_trace(const char*);
extern "C" void vita_stream_browser_trace_value(const char*, unsigned);
extern "C" void vita_stream_browser_log_message(const char*);
extern "C" void vita_stream_browser_log_performance(const char*, const char*);
extern "C" void vitagram_webkit_reset_trace();
namespace WebKit { void vitagramWebKitRenderingFreeze(unsigned); }

int main() {
    performance::startup_origin().store(123);
    performance::frame_context.frame = 77;
    const char* log = "ux0:data/vitagram/webapp.log";
    std::filesystem::create_directories("ux0:data/vitagram");
    std::filesystem::remove(log);
    diag_log::set_mask(0);
    vita_stream_browser_trace("webkit.network.schedule.received");
    webapp_diagnostics::recordUnsupported("web_app_setup_swipe_behavior");
    assert(!std::filesystem::exists(log));
    diag_log::set_mask(1u << static_cast<unsigned>(diag_log::Kind::Performance));
    vita_stream_browser_trace(nullptr);
    vita_stream_browser_trace("https://private.example/?token=SECRET");
    vita_stream_browser_trace_value("debug.network.send.prefix", 0x12345678);
    vita_stream_browser_log_message("SECRET");
    vita_stream_browser_log_performance("SECRET", "SECRET");
    vita_stream_browser_trace_value("webkit.curl.error", 0);
    vita_stream_browser_trace_value("webkit.curl.error", 256);
    vita_stream_browser_trace_value("webkit.curl.http_status", 99);
    vita_stream_browser_trace_value("webkit.curl.http_status", 600);
    WebKit::vitagramWebKitRenderingFreeze(512);
    assert(!std::filesystem::exists(log));
    std::vector<std::thread> writers;
    for (int i = 0; i < 8; ++i) writers.emplace_back([] {
        for (int n = 0; n < 100; ++n) vita_stream_browser_trace("webkit.network.schedule.received");
    });
    for (auto& writer : writers) writer.join();
    vita_stream_browser_trace("webkit.curl.transfer.failed");
    vita_stream_browser_trace("webkit.paint.proxy.damage.sent");
    vita_stream_browser_trace("webkit.paint.backing.ready");
    vitagram_webkit_reset_trace();
    vita_stream_browser_trace("webkit.network.schedule.received");
    webapp_diagnostics::recordUnsupported("web_app_setup_swipe_behavior");
    webapp_diagnostics::recordUnsupported("web_app_setup_secondary_button");
    webapp_diagnostics::recordUnsupported("SECRET");
    webapp_diagnostics::recordUnsupported(nullptr);
    vita_stream_browser_trace_value("webkit.curl.error", 60);
    vita_stream_browser_trace_value("webkit.curl.http_status", 403);
    WebKit::vitagramWebKitRenderingFreeze(1);
    WebKit::vitagramWebKitRenderingFreeze(129);
    WebKit::vitagramWebKitRenderingFreeze(0);
    diag_log::set_mask(0);
    vita_stream_browser_trace("webkit.curl.data.received");
    std::ifstream input(log);
    const std::string events{std::istreambuf_iterator<char>(input), {}};
    std::istringstream lines(events);
    std::string normalized, line;
    while (std::getline(lines, line)) {
        const auto metadata = line.find(" at_us=");
        assert(metadata != std::string::npos);
        assert(std::regex_match(line.substr(metadata), std::regex(" at_us=[0-9]+ session_us=123 frame=(0|77)")));
        normalized += line.substr(0, metadata) + "\n";
    }
    assert(events.find("frame=77") != std::string::npos && events.find("frame=0") != std::string::npos);
    assert(normalized == "stage=engine_trace code=1\nstage=engine_trace code=18\n"
                     "stage=engine_trace code=32\nstage=engine_trace code=33\nstage=engine_trace code=1\n"
                     "stage=bridge_unsupported_web_app_setup_swipe_behavior code=0\n"
                     "stage=bridge_unsupported_web_app_setup_secondary_button code=0\n"
                     "stage=bridge_unsupported_unknown code=0\n"
                     "stage=curl_error code=60\nstage=http_status code=403\n"
                     "stage=render_freeze code=1\nstage=render_freeze code=129\nstage=render_freeze code=0\n");
}
