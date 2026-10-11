// Host test for the TGS pre-check (src/tgs_policy.h): real stickers pass, what rlottie should never
// see is refused before it parses anything.
#include "tgs_policy.h"
#include <zlib.h>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

static std::string slurp(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

static std::string gunzip(const std::string& packed) {
    z_stream s{};
    assert(inflateInit2(&s, 16 + MAX_WBITS) == Z_OK);
    s.next_in = (Bytef*)packed.data(); s.avail_in = (uInt)packed.size();
    std::string out; char buf[8192]; int r;
    do { s.next_out = (Bytef*)buf; s.avail_out = sizeof(buf); r = inflate(&s, Z_NO_FLUSH); out.append(buf, sizeof(buf) - s.avail_out); } while (r == Z_OK);
    inflateEnd(&s);
    assert(r == Z_STREAM_END);
    return out;
}

int main(int argc, char** argv) {
    assert(argc >= 2);
    std::string why;
    const std::string packed = slurp(argv[1]);
    assert(!packed.empty() && packed.size() <= tgs_policy::kMaxPacked);
    const std::string sticker = gunzip(packed);
    assert(tgs_policy::acceptable(sticker, &why));

    const std::string base = R"({"v":"5.5.2","fr":60,"ip":0,"op":180,"w":512,"h":512,"assets":[],"layers":[{"ty":4,"ip":0,"op":180,"shapes":[]}]})";
    assert(tgs_policy::acceptable(base, &why));
    auto refused = [&](const std::string& json, const char* expected) {
        bool ok = tgs_policy::acceptable(json, &why);
        if (ok || why != expected) std::fprintf(stderr, "expected refusal '%s', got %s '%s'\n", expected, ok ? "accept" : "refusal", why.c_str());
        assert(!ok && why == expected);
    };
    refused(R"({"fr":60,"ip":0,"op":180,"w":512,"h":512,"layers":[{"ty":2,"ip":0,"op":180,"refId":"img"}]})", "image layer");
    refused(R"({"fr":60,"ip":0,"op":180,"w":512,"h":512,"assets":[{"id":"img","w":10,"h":10,"u":"","p":"data:image/png;base64,AAAA","e":1}],"layers":[]})", "embedded image");
    refused(R"({"fr":60,"ip":0,"op":180,"w":512,"h":512,"layers":[{"ty":4,"ip":0,"op":180,"ks":{"o":{"a":0,"k":100,"x":"var $bm_rt = 1;"}}}]})", "expression");
    refused(R"({"fr":60,"ip":0,"op":180,"w":20000,"h":512,"layers":[]})", "canvas size");
    refused(R"({"fr":1000,"ip":0,"op":180,"w":512,"h":512,"layers":[]})", "frame rate");
    refused(R"({"fr":60,"ip":0,"op":100000,"w":512,"h":512,"layers":[]})", "frame range");
    refused(R"({"fr":60,"w":512,"h":512,"layers":[]})", "missing w/h/fr/ip/op");
    refused("[1,2,3]", "not a JSON object or duplicate keys");
    // rlottie makes an asset an image whatever the order of fields, with "layers" or without "id" (F15).
    refused(R"({"w":512,"h":512,"fr":30,"ip":0,"op":60,"assets":[{"id":"raster","layers":[],"p":"data:image/png;base64,AAAA","e":1}],"layers":[]})", "embedded image");
    refused(R"({"w":512,"h":512,"fr":30,"ip":0,"op":60,"assets":[{"layers":[],"u":"","e":0}],"layers":[]})", "embedded image");
    refused(R"({"w":512,"h":512,"fr":30,"ip":0,"op":60,"assets":[{"id":"a","p":{"k":1}}],"layers":[]})", "embedded image");
    refused(R"({"w":512,"h":512,"fr":30,"ip":0,"op":60,"layers":[],"layers":[]})", "not a JSON object or duplicate keys");
    assert(tgs_policy::acceptable(R"({"w":512,"h":512,"fr":30,"ip":0,"op":60,"assets":[{"id":"comp","layers":[]}],"layers":[]})", &why));
    std::string deep = R"({"fr":60,"ip":0,"op":10,"w":512,"h":512,"layers":)";
    for (int i = 0; i < 200; ++i) deep += "[";
    for (int i = 0; i < 200; ++i) deep += "]";
    refused(deep + "}", "nested too deeply");
    std::string many = R"({"fr":60,"ip":0,"op":10,"w":512,"h":512,"layers":[)";
    for (int i = 0; i < 2100; ++i) many += std::string(i ? "," : "") + R"({"ty":3,"ip":0,"op":10})";
    refused(many + "]}", "too many layers");

    // Survey of rlottie's sample animations (not stickers): which would be refused, and why.
    std::map<std::string, int> reasons; int accepted = 0;
    for (int i = 2; i < argc; ++i) {
        std::string json = slurp(argv[i]);
        if (tgs_policy::acceptable(json, &why)) ++accepted; else ++reasons[why];
    }
    if (argc > 2) {
        std::printf("rlottie samples: %d accepted", accepted);
        for (auto& r : reasons) std::printf(", %d %s", r.second, r.first.c_str());
        std::printf("\n");
    }
    std::puts("TGS policy: real sticker accepted, image/expression/size/frame/depth/layer limits enforced: PASS");
}
