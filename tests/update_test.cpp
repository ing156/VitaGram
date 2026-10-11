#include "update_core.h"
#include <openssl/x509.h>
#include <unistd.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace update;

static std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

static void versions() {
    int v[3];
    assert(parse_version("v0.35.4", v) && v[0] == 0 && v[1] == 35 && v[2] == 4);
    assert(parse_version("1.2", v) && v[2] == 0);
    assert(!parse_version("", v) && !parse_version("1..2", v) && !parse_version("1.2.3.4", v) && !parse_version("1.2-beta", v) && !parse_version("v", v));
    assert(compare_versions("0.35.4", "0.35.10") < 0);
    assert(compare_versions("v0.36.0", "0.35.99") > 0);
    assert(compare_versions("0.35.4", "v0.35.4") == 0);
    assert(compare_versions("garbage", "0.35.4") == 0); // never "newer"
    assert(normalize_version("v1.0.0") == "1.0.0");
}

static void release() {
    Release r;
    std::string error;
    const char* good = R"({"tag_name":"v0.36.0","draft":false,"prerelease":false,"assets":[
        {"name":"notes.txt","browser_download_url":"https://x/notes.txt","size":1},
        {"name":"VitaGram-0.36.0.vpk.sig","browser_download_url":"https://x/s","size":71},
        {"name":"VitaGram-0.36.0.vpk","browser_download_url":"https://x/v","size":1234}]})";
    assert(parse_release(good, r, error));
    assert(r.version == "0.36.0" && r.vpk_url == "https://x/v" && r.sig_url == "https://x/s" && r.vpk_size == 1234);
    assert(!parse_release(R"({"tag_name":"v1.0.0","prerelease":true,"assets":[]})", r, error));
    assert(!parse_release(R"({"tag_name":"v1.0.0","draft":true,"assets":[]})", r, error));
    assert(!parse_release(R"({"tag_name":"nightly","assets":[]})", r, error));
    assert(!parse_release(R"({"tag_name":"v1.0.0","assets":[{"name":"a.vpk","browser_download_url":"https://x/v"}]})", r, error)); // unsigned
    assert(!parse_release(R"({"tag_name":"v1.0.0","assets":[{"name":"a.vpk","browser_download_url":"u"},{"name":"b.vpk.sig","browser_download_url":"s"}]})", r, error)); // sig of another file
    assert(!parse_release("not json", r, error));
}

static void urls() {
    Url u;
    assert(split_https_url("https://api.github.com/repos/a/b/releases/latest", u) && u.host == "api.github.com" && u.port == "443" && u.path == "/repos/a/b/releases/latest");
    assert(split_https_url("https://host:8443", u) && u.port == "8443" && u.path == "/");
    assert(split_https_url("https://h/p?q=1#frag", u) && u.path == "/p?q=1");
    assert(!split_https_url("http://github.com/x", u));
    assert(!split_https_url("https://user@evil/x", u));
    assert(!split_https_url("https://a b/x", u));
    assert(!split_https_url("https://host/pa th", u));
    Url base{"github.com", "443", "/x"};
    std::string out;
    assert(resolve_location(base, "https://objects.githubusercontent.com/y", out) && out == "https://objects.githubusercontent.com/y");
    assert(resolve_location(base, "/z", out) && out == "https://github.com/z");
    assert(!resolve_location(base, "http://github.com/z", out));
    assert(!resolve_location(base, "//evil/z", out));
}

static std::string collect(HttpResponse& r, const std::string& wire, size_t step, bool& ok) {
    std::string body;
    ok = true;
    for (size_t i = 0; i < wire.size() && ok; i += step)
        ok = r.feed(wire.data() + i, std::min(step, wire.size() - i), [&](const char* d, size_t n) { body.append(d, n); return true; });
    return body;
}

static void http() {
    const std::string chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX: y\r\n\r\n5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nT: v\r\n\r\n";
    const std::string sized = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello worldEXTRA";
    for (size_t step : {1u, 3u, 7u, 1000u}) {
        bool ok; HttpResponse a; assert(collect(a, chunked, step, ok) == "hello world" && ok && a.complete() && a.status() == 200);
        HttpResponse b; assert(collect(b, sized, step, ok) == "hello world" && ok && b.complete() && b.contentLength() == 11);
    }
    bool ok;
    HttpResponse redirect;
    collect(redirect, "HTTP/1.1 302 Found\r\nLocation: https://x/y\r\nContent-Length: 0\r\n\r\n", 5, ok);
    assert(ok && redirect.complete() && redirect.status() == 302 && redirect.location() == "https://x/y");
    HttpResponse cut;
    assert(collect(cut, "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nshort", 4, ok) == "short" && !cut.complete());
    HttpResponse close_framed;
    collect(close_framed, "HTTP/1.0 200 OK\r\n\r\nabc", 4, ok);
    assert(close_framed.bodyIsUntilClose() && !close_framed.complete());
    HttpResponse bad;
    collect(bad, "garbage\r\n\r\n", 100, ok);
    assert(!ok);
    HttpResponse bad_chunk;
    collect(bad_chunk, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 100, ok);
    assert(!ok);
    HttpResponse aborted;
    assert(!aborted.feed("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc", 41, [](const char*, size_t) { return false; }));
}

static void zip(const std::string& dir) {
    std::string error;
    uint64_t last_total = 0;
    assert(extract_zip(dir + "/good.zip", dir + "/out", [&](uint64_t, uint64_t total) { last_total = total; return true; }, error));
    assert(slurp(dir + "/out/eboot.bin") == "EBOOT");
    assert(slurp(dir + "/out/sce_sys/param.sfo") == std::string(100000, 'p')); // deflated, > one buffer
    assert(slurp(dir + "/out/lang/en.lang") == "stored");
    assert(last_total == 5 + 100000 + 6);
    assert(!extract_zip(dir + "/slip.zip", dir + "/out2", nullptr, error) && error.find("unsafe") != std::string::npos);
    assert(!extract_zip(dir + "/corrupt.zip", dir + "/out3", nullptr, error));
    assert(!extract_zip(dir + "/missing.zip", dir + "/out4", nullptr, error));
    assert(!extract_zip(dir + "/good.zip", dir + "/out5", [](uint64_t, uint64_t) { return false; }, error) && error == "cancelled");
    assert(!safe_entry_name("../x") && !safe_entry_name("/x") && !safe_entry_name("a/../b") && !safe_entry_name("a\\b") &&
           !safe_entry_name("C:x") && !safe_entry_name(""));
    assert(safe_entry_name("sce_sys/package/head.bin") && safe_entry_name("dir/") && safe_entry_name("a..b"));
    assert(remove_tree(dir + "/out") && slurp(dir + "/out/eboot.bin").empty());
}

static void signature(const std::string& dir) {
    const std::string pub = slurp(dir + "/pub.pem"), vpk = slurp(dir + "/good.zip"), sig = slurp(dir + "/good.sig");
    auto verify = [&](const std::string& version, const std::string& data, const std::string& signature_bytes, size_t step) {
        SignatureVerifier v;
        if (!v.begin(version, pub.c_str())) return false;
        for (size_t i = 0; i < data.size(); i += step) v.update(data.data() + i, std::min(step, data.size() - i));
        return v.finish((const unsigned char*)signature_bytes.data(), signature_bytes.size());
    };
    assert(verify("9.9.9", vpk, sig, 1000));
    assert(verify("9.9.9", vpk, sig, 1));                 // chunking does not matter
    assert(!verify("9.9.8", vpk, sig, 4096));              // version is bound
    std::string tampered = vpk; tampered[tampered.size() / 2] ^= 1;
    assert(!verify("9.9.9", tampered, sig, 4096));
    std::string bad_sig = sig; bad_sig[10] ^= 1;
    assert(!verify("9.9.9", vpk, bad_sig, 4096));
    assert(!verify("9.9.9", vpk, "", 4096));
    SignatureVerifier garbage_key;
    assert(!garbage_key.begin("1.0.0", "not a key"));
    SignatureVerifier builtin; // the shipped public key must at least parse
    assert(builtin.begin("1.0.0"));
}

// Cleanup never follows a link out of the tree; ZIP limits hold before anything is written (F08, F10).
static void hardening(const std::string& dir) {
    const std::string tree = dir + "/cleanup", outside = dir + "/outside";
    assert(make_directories(tree + "/sub") && make_directories(outside));
    std::ofstream(outside + "/keep") << "keep";
    assert(symlink(outside.c_str(), (tree + "/sub/link").c_str()) == 0);
    assert(remove_tree(tree));
    assert(slurp(outside + "/keep") == "keep" && access(tree.c_str(), F_OK) != 0);

    std::string error;
    assert(!extract_zip(dir + "/underreported.zip", dir + "/under", nullptr, error));
    assert(error.find("larger than declared") != std::string::npos);
    assert(access((dir + "/under/blob").c_str(), F_OK) != 0); // the partial output is removed
}

// The updater's CA bundle: loaded certificate by certificate; a damaged entry costs only itself.
static void ca_bundle(const std::string& dir) {
    const std::string bundle = slurp(dir + "/cacert.pem");
    assert(!bundle.empty());
    std::string error;
    X509_STORE* store = X509_STORE_new();
    const int all = load_ca_bundle(store, dir + "/cacert.pem", error);
    assert(all > 100 && error.empty());
    X509_STORE_free(store);

    // Corrupt the base64 body of the second certificate: every other one still loads.
    std::string damaged = bundle;
    size_t second = damaged.find("-----BEGIN CERTIFICATE-----", damaged.find("-----END CERTIFICATE-----"));
    assert(second != std::string::npos);
    damaged.replace(second + 28, 40, std::string(40, '!'));
    std::ofstream(dir + "/damaged.pem", std::ios::binary) << damaged;
    store = X509_STORE_new();
    assert(load_ca_bundle(store, dir + "/damaged.pem", error) == all - 1 && error.empty());
    X509_STORE_free(store);

    std::ofstream(dir + "/none.pem", std::ios::binary) << "# no certificates here\n";
    store = X509_STORE_new();
    assert(load_ca_bundle(store, dir + "/none.pem", error) == 0 && error.find("0 certificates") != std::string::npos);
    assert(load_ca_bundle(store, dir + "/missing.pem", error) == 0 && error.find("cannot open") != std::string::npos);
    X509_STORE_free(store);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    versions(); release(); urls(); http(); zip(argv[1]); signature(argv[1]); ca_bundle(argv[1]); hardening(argv[1]);
    std::puts("update: PASS");
    return 0;
}
