#pragma once
#include "media_limits.h"
#include <jansson.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct VideoQuality {
    json_t* file=nullptr; // Borrowed from the message JSON.
    int64_t width=0, height=0;
    bool alternative=false;
    static int64_t number(json_t* o,const char* k) { return json_integer_value(json_object_get(o,k)); }
    static int64_t size(json_t* f) { return std::max(number(f,"size"),number(f,"expected_size")); }
    static bool hardwareRasterSupported(int64_t width, int64_t height) {
        return width > 0 && height > 0 && width <= 1280 && height <= 1088 &&
            width * height <= 1280LL * 720;
    }
    static bool withinLimit(int64_t width, int64_t height, int preference) {
        const int limits[] = {0, 360, 480, 720};
        const int limit = limits[std::clamp(preference, 0, 3)];
        return !limit || (std::min(width, height) <= limit &&
            std::max(width, height) <= (limit == 360 ? 640 : limit == 480 ? 854 : 1280));
    }
    static const char* exclusion(const VideoQuality& q, int maximum) {
        json_t* local = json_object_get(q.file, "local");
        const char* path = json_string_value(json_object_get(local, "path"));
        const bool cached = json_is_true(json_object_get(local, "is_downloading_completed")) && path && *path;
        if (!cached && number(q.file, "id") <= 0) return "file_unavailable";
        if (!hardwareRasterSupported(q.width, q.height)) return "hardware_raster";
        if (size(q.file) > media_limits::video_bytes) return "file_over_1GiB";
        if (!withinLimit(q.width, q.height, maximum)) return "quality_limit";
        return nullptr;
    }
    static bool usable(const VideoQuality& q) { return !exclusion(q, 0); }
    static bool h264(json_t* video) {
        const char* raw = json_string_value(json_object_get(video, "codec"));
        std::string codec = raw ? raw : "";
        std::transform(codec.begin(), codec.end(), codec.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return codec == "h264";
    }
    static std::vector<VideoQuality> available(json_t* content, int maximum) {
        if (!maximum) {
            auto q = select(content);
            return usable(q) ? std::vector<VideoQuality>{q} : std::vector<VideoQuality>{};
        }
        std::vector<VideoQuality> result;
        auto add = [&](json_t* video, bool alternative) {
            VideoQuality q{json_object_get(video, "video"), number(video, "width"), number(video, "height"), alternative};
            if (!exclusion(q, maximum)) result.push_back(q);
        };
        add(json_object_get(content, "video"), false);
        size_t index; json_t* video;
        json_array_foreach(json_object_get(content, "alternative_videos"), index, video)
            if (h264(video)) add(video, true);
        std::sort(result.begin(), result.end(), [](const VideoQuality& a, const VideoQuality& b) {
            if (a.width * a.height != b.width * b.height) return a.width * a.height < b.width * b.height;
            if (a.width != b.width) return a.width < b.width;
            const int64_t sa = size(a.file) > 0 ? size(a.file) : INT64_MAX;
            const int64_t sb = size(b.file) > 0 ? size(b.file) : INT64_MAX;
            if (sa != sb) return sa < sb;
            return a.alternative > b.alternative;
        });
        result.erase(std::unique(result.begin(), result.end(), [](const VideoQuality& a, const VideoQuality& b) {
            return a.width == b.width && a.height == b.height;
        }), result.end());
        return result;
    }
    static std::string diagnostic(json_t* content, int maximum) {
        std::string report;
        const auto selected = select(content, maximum);
        auto write = [&](json_t* video, bool alternative) {
            VideoQuality q{json_object_get(video, "video"), number(video, "width"), number(video, "height"), alternative};
            const char* reason = alternative && !h264(video) ? "unsupported_codec" : exclusion(q, maximum);
            if (!reason && !maximum && q.file != selected.file) reason = "minimum_mode";
            char line[256];
            std::snprintf(line, sizeof(line), "variant source=%s width=%lld height=%lld bytes=%lld h264=%d result=%s selected=%d\n",
                alternative ? "alternative" : "original", (long long)q.width, (long long)q.height,
                (long long)size(q.file), alternative && h264(video), reason ? reason : "allowed", q.file == selected.file);
            // Bound diagnostic memory even if a message contains an unusually large variant list.
            if (report.size() + std::strlen(line) > 8192 - 32) return false;
            report += line;
            return true;
        };
        write(json_object_get(content, "video"), false);
        size_t index; json_t* video;
        json_array_foreach(json_object_get(content, "alternative_videos"), index, video)
            if (!write(video, true)) { report += "omitted_variants=1\n"; break; }
        return report;
    }
    static void diagnose(FILE* log, json_t* content, int maximum) {
        const auto report = diagnostic(content, maximum);
        std::fwrite(report.data(), 1, report.size(), log);
    }
    static VideoQuality select(json_t* content, int preference = 0) {
        json_t* original=json_object_get(content,"video");
        VideoQuality best{json_object_get(original,"video"),number(original,"width"),number(original,"height"),false};
        auto area=[](const VideoQuality& q) {
            return hardwareRasterSupported(q.width, q.height) ? q.width*q.height : INT64_MAX;
        };
        auto preferred = [&](const VideoQuality& q) {
            return preference > 0 && usable(q) && withinLimit(q.width, q.height, preference);
        };
        size_t index; json_t* v;
        json_array_foreach(json_object_get(content,"alternative_videos"),index,v) {
            // Telegram video alternatives must keep the hardware H.264 path.
            if (!h264(v)) continue;
            VideoQuality candidate{json_object_get(v,"video"),number(v,"width"),number(v,"height"),true};
            if (!usable(candidate)) continue;
            int64_t a=area(candidate), b=area(best);
            const int64_t sa = size(candidate.file) > 0 ? size(candidate.file) : INT64_MAX;
            const int64_t sb = size(best.file) > 0 ? size(best.file) : INT64_MAX;
            const bool cp = preferred(candidate), bp = preferred(best);
            if (!usable(best) || (cp != bp ? cp : (cp ? a>b : a<b)) ||
                (cp == bp && a==b && (sa<sb || (!best.alternative && sa==sb)))) best=candidate;
        }
        return best;
    }
};
