#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// UI-owned: exact text/font/scale keys, bounded retained strings and entries.
class TextWidthCache {
public:
    template<class Measure> int get(const char* text, uintptr_t font, float scale, Measure measure) {
        const size_t length=std::strlen(text);
        if (!length) return 0;
        if (length>4096) return measure();
        ++clock_;
        for (auto& e:entries_) if (e.font==font && e.scale==scale && e.text.size()==length && e.text.compare(0,length,text,length)==0) { e.used=clock_; return e.width; }
        const int width=measure();
        while (!entries_.empty() && (entries_.size()>=128 || bytes_+length>128*1024)) {
            auto oldest=std::min_element(entries_.begin(),entries_.end(),[](const Entry& a,const Entry& b){return a.used<b.used;});
            bytes_-=oldest->text.size();entries_.erase(oldest);
        }
        entries_.push_back({text,font,scale,width,clock_});bytes_+=length;
        return width;
    }
    void clear() { entries_.clear();bytes_=0; }
    size_t size() const { return entries_.size(); }
    size_t bytes() const { return bytes_; }
private:
    struct Entry { std::string text;uintptr_t font;float scale;int width;uint64_t used; };
    std::vector<Entry> entries_;
    size_t bytes_=0;uint64_t clock_=0;
};
// Measurements use complete strings, preserving kerning and fallback fonts.
// Prefix boundaries match the previous UTF-8 trimming rule.
template<class Measure> std::string fit_utf8_text(const std::string& text,int width,Measure measure) {
    if (measure(text)<=width) return text;
    std::vector<size_t> ends{0};
    for (size_t i=0;i<text.size();) {
        ++i;while (i<text.size() && (static_cast<unsigned char>(text[i])&0xc0)==0x80) ++i;
        ends.push_back(i);
    }
    size_t lo=0,hi=ends.size()-1;
    while (lo<hi) {
        size_t mid=lo+(hi-lo+1)/2;
        if (measure(text.substr(0,ends[mid])+"...")<=width) lo=mid;
        else hi=mid-1;
    }
    return text.substr(0,ends[lo])+"...";
}
