#pragma once
#include <jansson.h>
#include <string>
#include <cstring>
#include <string_view>

// Grammar-only scan of an unused TDLib update. No DOM for the discarded body;
// any extra, ambiguous type or malformed syntax falls back to ordinary Jansson.
namespace td_unused_detail {
struct Reader {
    const char* p;
    const char* end;
    void ws() { while (p<end && (*p==' ' || *p=='\n' || *p=='\r' || *p=='\t')) ++p; }
    bool string() {
        if (p==end || *p++!='"') return false;
        while (p<end) {
            unsigned char c=*p++;
            if (c=='"') return true;
            if (c<32) return false;
            if (c>=128) {
                unsigned count=0;unsigned code=0, minimum=0;
                if (c>=0xc2 && c<=0xdf) { count=1;code=c&31;minimum=0x80; }
                else if (c>=0xe0 && c<=0xef) { count=2;code=c&15;minimum=0x800; }
                else if (c>=0xf0 && c<=0xf4) { count=3;code=c&7;minimum=0x10000; }
                else return false;
                for (unsigned i=0;i<count;++i) {
                    if (p==end || (static_cast<unsigned char>(*p)&0xc0)!=0x80) return false;
                    code=(code<<6)|(static_cast<unsigned char>(*p++)&63);
                }
                if (code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff)) return false;
            }
            if (c=='\\') {
                if (p==end) return false;
                c=*p++;
                if (c=='u') {
                    auto hex4=[&]() -> int {
                        int code=0;
                        for (int i=0;i<4;++i) {
                            if (p==end) return -1;
                            unsigned char h=*p++;int digit=h>='0' && h<='9' ? h-'0' : h>='a' && h<='f' ? h-'a'+10 : h>='A' && h<='F' ? h-'A'+10 : -1;
                            if (digit<0) return -1;
                            code=code*16+digit;
                        }
                        return code;
                    };
                    const int code=hex4();
                    if (code<=0) return false;
                    if (code>=0xd800 && code<=0xdbff) {
                        if (end-p<2 || p[0]!='\\' || p[1]!='u') return false;
                        p+=2;const int low=hex4();if (low<0xdc00 || low>0xdfff) return false;
                    } else if (code>=0xdc00 && code<=0xdfff) return false;
                } else if (!c || !std::strchr("\"\\/bfnrt",c)) return false;
            }
        }
        return false;
    }
    bool value(unsigned depth=0) {
        ws();if (p==end || depth>64) return false;
        if (*p=='"') return string();
        if (*p=='{' || *p=='[') {
            const bool object=*p++=='{';const char close=object ? '}' : ']';ws();
            if (p<end && *p==close) { ++p;return true; }
            for (;;) {
                if (object) { if (!string()) return false;ws();if (p==end || *p++!=':') return false; }
                if (!value(depth+1)) return false;
                ws();if (p==end) return false;
                if (*p==close) { ++p;return true; }
                if (*p++!=',') return false;
                ws();
            }
        }
        for (const char* literal:{"true","false","null"}) {
            const size_t n=std::strlen(literal);
            if (static_cast<size_t>(end-p)>=n && !std::memcmp(p,literal,n)) { p+=n;return true; }
        }
        if (*p=='-') ++p;
        if (p==end) return false;
        if (*p=='0') ++p;
        else {
            if (*p<'1' || *p>'9') return false;
            do { ++p; } while (p<end && *p>='0' && *p<='9');
        }
        if (p<end && *p=='.') {
            ++p;const char* start=p;while (p<end && *p>='0' && *p<='9') ++p;if (start==p) return false;
        }
        if (p<end && (*p=='e' || *p=='E')) {
            ++p;if (p<end && (*p=='+' || *p=='-')) ++p;
            const char* start=p;while (p<end && *p>='0' && *p<='9') ++p;if (start==p) return false;
        }
        return true;
    }
};
}
inline json_t* compact_raw_unused_td_update(const std::string& text) {
    // TDLib emits @type near the beginning. A missed candidate only loses the
    // optimization; it still takes the full parser path.
    if (text.compare(0,1,"{") || std::string_view(text.data(),text.size()<128 ? text.size() : 128).find("updateStickerSet")==std::string_view::npos) return nullptr;
    td_unused_detail::Reader r{text.data(),text.data()+text.size()};
    ++r.p;r.ws();bool seen=false, matches=false;
    for (;;) {
        const char* begin=r.p;if (!r.string() || r.p-begin>256) return nullptr;
        auto* key=json_loadb(begin,r.p-begin,JSON_DECODE_ANY,nullptr);
        const char* name=json_string_value(key);
        const bool is_type=name && !std::strcmp(name,"@type");
        const bool extra=name && !std::strcmp(name,"@extra");
        json_decref(key);if (extra || (is_type && seen)) return nullptr;
        r.ws();if (r.p==r.end || *r.p++!=':') return nullptr;r.ws();begin=r.p;
        if (!r.value()) return nullptr;
        if (is_type) {
            seen=true;auto* type=json_loadb(begin,r.p-begin,JSON_DECODE_ANY,nullptr);
            const char* value=json_string_value(type);matches=value && !std::strcmp(value,"updateStickerSet");json_decref(type);
            if (!matches) return nullptr;
        }
        r.ws();if (r.p==r.end) return nullptr;
        if (*r.p=='}') { ++r.p;break; }
        if (*r.p++!=',') return nullptr;
        r.ws();
    }
    r.ws();if (r.p!=r.end || !seen || !matches) return nullptr;
    return json_pack("{s:s}","@type","updateStickerSet");
}
