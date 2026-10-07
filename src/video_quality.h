#pragma once
#include <jansson.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

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
    static VideoQuality minimum(json_t* content) {
        json_t* original=json_object_get(content,"video");
        VideoQuality best{json_object_get(original,"video"),number(original,"width"),number(original,"height"),false};
        auto area=[](const VideoQuality& q) {
            return hardwareRasterSupported(q.width, q.height) ? q.width*q.height : INT64_MAX;
        };
        auto usable=[&](const VideoQuality& q) {
            json_t* local=json_object_get(q.file,"local");
            const char* path=json_string_value(json_object_get(local,"path"));
            bool cached=json_is_true(json_object_get(local,"is_downloading_completed")) && path && *path;
            return (cached || number(q.file,"id")>0) && area(q)<=1280*720 && size(q.file)<=128LL*1024*1024;
        };
        size_t index; json_t* v;
        json_array_foreach(json_object_get(content,"alternative_videos"),index,v) {
            const char* raw=json_string_value(json_object_get(v,"codec"));
            std::string codec=raw ? raw : "";
            std::transform(codec.begin(),codec.end(),codec.begin(),[](unsigned char c){return (char)std::tolower(c);});
            // Telegram video alternatives must keep the hardware H.264 path.
            if (codec!="h264") continue;
            VideoQuality candidate{json_object_get(v,"video"),number(v,"width"),number(v,"height"),true};
            if (!usable(candidate)) continue;
            int64_t a=area(candidate), b=area(best), sa=size(candidate.file), sb=size(best.file);
            if (!usable(best) || a<b || (a==b && ((sa>0 && sb>0 && sa<sb) || (!best.alternative && (!sa || !sb || sa==sb))))) best=candidate;
        }
        return best;
    }
};
