#include "update_core.h"
#include "update_key.h"
#include <jansson.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <zlib.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace update {

bool parse_version(const std::string& text, int out[3]) {
    size_t i = (!text.empty() && (text[0] == 'v' || text[0] == 'V')) ? 1 : 0;
    int parts = 0;
    out[0] = out[1] = out[2] = 0;
    while (i < text.size()) {
        if (parts >= 3 || !std::isdigit((unsigned char)text[i])) return false;
        long value = 0;
        size_t digits = 0;
        while (i < text.size() && std::isdigit((unsigned char)text[i])) {
            value = value * 10 + (text[i++] - '0');
            if (++digits > 6) return false;
        }
        out[parts++] = (int)value;
        if (i < text.size()) { if (text[i] != '.' || i + 1 == text.size()) return false; ++i; }
    }
    return parts > 0;
}

std::string normalize_version(const std::string& text) {
    return (!text.empty() && (text[0] == 'v' || text[0] == 'V')) ? text.substr(1) : text;
}

int compare_versions(const std::string& a, const std::string& b) {
    int x[3], y[3];
    if (!parse_version(a, x) || !parse_version(b, y)) return 0;
    for (int i = 0; i < 3; ++i) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

bool parse_release(const std::string& json, Release& out, std::string& error) {
    json_error_t jerror;
    json_t* root = json_loadb(json.data(), json.size(), 0, &jerror);
    if (!json_is_object(root)) { if (root) json_decref(root); error = "release JSON is invalid"; return false; }
    bool ok = false;
    do {
        if (json_is_true(json_object_get(root, "draft")) || json_is_true(json_object_get(root, "prerelease"))) {
            error = "release is a draft or pre-release"; break;
        }
        const char* tag = json_string_value(json_object_get(root, "tag_name"));
        int parsed[3];
        if (!tag || !parse_version(tag, parsed)) { error = "release tag is not a version"; break; }
        Release release;
        release.version = normalize_version(tag);
        json_t* assets = json_object_get(root, "assets");
        std::string sig_name;
        size_t index;
        json_t* asset;
        json_array_foreach(assets, index, asset) {
            const char* name = json_string_value(json_object_get(asset, "name"));
            const char* url = json_string_value(json_object_get(asset, "browser_download_url"));
            if (!name || !url) continue;
            std::string n = name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".vpk") == 0 && release.vpk_url.empty()) {
                release.vpk_name = n; release.vpk_url = url;
                json_t* size = json_object_get(asset, "size");
                release.vpk_size = json_is_integer(size) ? (uint64_t)json_integer_value(size) : 0;
            }
        }
        if (release.vpk_url.empty()) { error = "release has no .vpk asset"; break; }
        sig_name = release.vpk_name + ".sig";
        json_array_foreach(assets, index, asset) {
            const char* name = json_string_value(json_object_get(asset, "name"));
            const char* url = json_string_value(json_object_get(asset, "browser_download_url"));
            if (name && url && sig_name == name) release.sig_url = url;
        }
        if (release.sig_url.empty()) { error = "release has no signature (.vpk.sig)"; break; }
        out = release;
        ok = true;
    } while (false);
    json_decref(root);
    return ok;
}

bool split_https_url(const std::string& url, Url& out) {
    static const char scheme[] = "https://";
    if (url.compare(0, sizeof(scheme) - 1, scheme) != 0) return false;
    size_t host_start = sizeof(scheme) - 1;
    size_t path_start = url.find_first_of("/?#", host_start);
    std::string authority = url.substr(host_start, path_start == std::string::npos ? std::string::npos : path_start - host_start);
    std::string path = path_start == std::string::npos ? "/" : url.substr(path_start);
    if (path[0] != '/') path = "/" + path;
    size_t fragment = path.find('#');
    if (fragment != std::string::npos) path.resize(fragment);
    if (authority.empty() || authority.find('@') != std::string::npos || authority[0] == '[') return false;
    size_t colon = authority.find(':');
    out.host = authority.substr(0, colon);
    out.port = colon == std::string::npos ? "443" : authority.substr(colon + 1);
    out.path = path;
    if (out.host.empty() || out.port.empty() || out.port.size() > 5) return false;
    for (char c : out.port) if (!std::isdigit((unsigned char)c)) return false;
    for (char c : out.host) if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-')) return false;
    for (char c : out.path) if ((unsigned char)c <= 0x20 || c == 0x7f) return false;
    return true;
}

bool resolve_location(const Url& base, const std::string& location, std::string& absolute) {
    if (location.compare(0, 8, "https://") == 0) { absolute = location; return true; }
    if (!location.empty() && location[0] == '/' && location.compare(0, 2, "//") != 0) {
        absolute = "https://" + base.host + (base.port == "443" ? "" : ":" + base.port) + location;
        return true;
    }
    return false; // plain http:// and anything odd: refuse to leave TLS
}

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
bool parse_hex(const std::string& s, uint64_t& value) {
    if (s.empty() || s.size() > 12) return false;
    value = 0;
    for (char c : s) {
        int d = std::isdigit((unsigned char)c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        value = value * 16 + d;
    }
    return true;
}
} // namespace

bool HttpResponse::parseHeaders() {
    size_t end = buffer_.find("\r\n\r\n");
    if (end == std::string::npos) return buffer_.size() < 32768; // header block too large -> error
    std::string head = buffer_.substr(0, end);
    buffer_.erase(0, end + 4);
    size_t line_end = head.find("\r\n");
    std::string status_line = head.substr(0, line_end);
    if (status_line.compare(0, 5, "HTTP/") != 0 || status_line.size() < 12) return false;
    status_ = std::atoi(status_line.c_str() + status_line.find(' ') + 1);
    if (status_ < 100) return false;
    while (line_end != std::string::npos) {
        size_t next = head.find("\r\n", line_end + 2);
        std::string line = head.substr(line_end + 2, next == std::string::npos ? std::string::npos : next - line_end - 2);
        line_end = next;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = lower(line.substr(0, colon)), value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        if (key == "content-length") content_length_ = std::strtoll(value.c_str(), nullptr, 10);
        else if (key == "location") location_ = value;
        else if (key == "transfer-encoding" && lower(value).find("chunked") != std::string::npos) chunked_ = true;
    }
    if (status_ == 204 || status_ == 304 || (status_ >= 100 && status_ < 200) || content_length_ == 0) state_ = State::Done;
    else state_ = chunked_ ? State::ChunkSize : State::Body;
    return true;
}

bool HttpResponse::feed(const char* data, size_t size, const Sink& sink) {
    buffer_.append(data, size);
    for (;;) {
        switch (state_) {
        case State::Headers:
            if (!parseHeaders()) return false;
            if (state_ == State::Headers) return true;
            break;
        case State::Body: {
            size_t take = buffer_.size();
            if (content_length_ >= 0) take = (size_t)std::min<uint64_t>(take, (uint64_t)content_length_ - body_bytes_);
            if (take) {
                if (!sink(buffer_.data(), take)) return false;
                body_bytes_ += take;
                buffer_.erase(0, take);
            }
            if (content_length_ >= 0 && body_bytes_ >= (uint64_t)content_length_) state_ = State::Done;
            return true;
        }
        case State::ChunkSize: {
            size_t eol = buffer_.find("\r\n");
            if (eol == std::string::npos) return buffer_.size() < 64;
            std::string line = buffer_.substr(0, eol);
            line.resize(line.find(';') == std::string::npos ? line.size() : line.find(';'));
            if (!parse_hex(line, chunk_left_)) return false;
            buffer_.erase(0, eol + 2);
            state_ = chunk_left_ ? State::ChunkData : State::Trailer;
            break;
        }
        case State::ChunkData: {
            size_t take = (size_t)std::min<uint64_t>(buffer_.size(), chunk_left_);
            if (take) {
                if (!sink(buffer_.data(), take)) return false;
                body_bytes_ += take;
                chunk_left_ -= take;
                buffer_.erase(0, take);
            }
            if (chunk_left_) return true;
            state_ = State::ChunkDataEnd;
            break;
        }
        case State::ChunkDataEnd:
            if (buffer_.size() < 2) return true;
            if (buffer_.compare(0, 2, "\r\n") != 0) return false;
            buffer_.erase(0, 2);
            state_ = State::ChunkSize;
            break;
        case State::Trailer: {
            size_t eol = buffer_.find("\r\n");
            if (eol == std::string::npos) return buffer_.size() < 8192;
            bool empty = eol == 0;
            buffer_.erase(0, eol + 2);
            if (empty) { state_ = State::Done; return true; }
            break;
        }
        case State::Done:
            buffer_.clear();
            return true;
        }
    }
}

struct SignatureVerifier::Impl {
    EVP_MD_CTX* ctx = nullptr;
    EVP_PKEY* key = nullptr;
};

SignatureVerifier::SignatureVerifier() : impl_(new Impl) {}
SignatureVerifier::~SignatureVerifier() {
    if (impl_->ctx) EVP_MD_CTX_destroy(impl_->ctx);
    if (impl_->key) EVP_PKEY_free(impl_->key);
    delete impl_;
}

bool SignatureVerifier::begin(const std::string& version, const char* public_key_pem) {
    const char* pem = public_key_pem ? public_key_pem : VITAGRAM_UPDATE_PUBLIC_KEY;
    BIO* bio = BIO_new_mem_buf((void*)pem, -1);
    if (!bio) return false;
    impl_->key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!impl_->key) return false;
    impl_->ctx = EVP_MD_CTX_create();
    if (!impl_->ctx || EVP_DigestVerifyInit(impl_->ctx, nullptr, EVP_sha256(), nullptr, impl_->key) != 1) return false;
    std::string prefix = "vitagram-update-v1\n" + version + "\n";
    return EVP_DigestVerifyUpdate(impl_->ctx, prefix.data(), prefix.size()) == 1;
}

void SignatureVerifier::update(const void* data, size_t size) {
    if (impl_->ctx && size) EVP_DigestVerifyUpdate(impl_->ctx, data, size);
}

bool SignatureVerifier::finish(const unsigned char* signature, size_t size) {
    if (!impl_->ctx || !size) return false;
    return EVP_DigestVerifyFinal(impl_->ctx, signature, size) == 1;
}

bool safe_entry_name(const std::string& name) {
    if (name.empty() || name.size() > 240 || name[0] == '/') return false;
    size_t start = 0;
    while (start <= name.size()) {
        size_t slash = name.find('/', start);
        std::string part = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    for (char c : name) if (c == '\\' || c == ':' || (unsigned char)c < 0x20) return false;
    return true;
}

bool make_directories(const std::string& path) {
    std::string current;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!current.empty() && current.back() != ':') {
                if (mkdir(current.c_str(), 0777) != 0 && errno != EEXIST) {
                    struct stat st;
                    if (stat(current.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
                }
            }
        }
        if (i < path.size()) current += path[i];
    }
    return true;
}

bool remove_tree(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return true;
    if (!S_ISDIR(st.st_mode)) return std::remove(path.c_str()) == 0;
    if (DIR* dir = opendir(path.c_str())) {
        std::vector<std::string> names;
        while (dirent* entry = readdir(dir)) {
            std::string name = entry->d_name;
            if (name != "." && name != "..") names.push_back(name);
        }
        closedir(dir);
        for (const auto& name : names) remove_tree(path + "/" + name);
    }
    return rmdir(path.c_str()) == 0;
}

namespace {
uint16_t u16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t u32(const unsigned char* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

struct ZipEntry { std::string name; uint16_t method; uint32_t crc, csize, usize, offset; };
}

bool extract_zip(const std::string& zip_path, const std::string& dest,
                 const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error) {
    FILE* zip = std::fopen(zip_path.c_str(), "rb");
    if (!zip) { error = "cannot open the package"; return false; }
    struct Closer { FILE* f; ~Closer() { std::fclose(f); } } closer{zip};
    std::fseek(zip, 0, SEEK_END);
    long file_size = std::ftell(zip);
    if (file_size < 22) { error = "package is too small"; return false; }
    long tail = std::min<long>(file_size, 22 + 65535);
    std::vector<unsigned char> eocd_buf(tail);
    std::fseek(zip, file_size - tail, SEEK_SET);
    if (std::fread(eocd_buf.data(), 1, tail, zip) != (size_t)tail) { error = "package read failed"; return false; }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; --i) if (u32(&eocd_buf[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { error = "package is not a zip archive"; return false; }
    uint32_t count = u16(&eocd_buf[eocd + 10]), cd_size = u32(&eocd_buf[eocd + 12]), cd_offset = u32(&eocd_buf[eocd + 16]);
    if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu) { error = "zip64 packages are not supported"; return false; }
    if ((uint64_t)cd_offset + cd_size > (uint64_t)file_size) { error = "package directory is damaged"; return false; }
    std::vector<unsigned char> cd(cd_size);
    std::fseek(zip, cd_offset, SEEK_SET);
    if (cd_size && std::fread(cd.data(), 1, cd_size, zip) != cd_size) { error = "package read failed"; return false; }

    std::vector<ZipEntry> entries;
    uint64_t total = 0;
    size_t pos = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 46 > cd.size() || u32(&cd[pos]) != 0x02014b50) { error = "package directory is damaged"; return false; }
        ZipEntry e;
        uint16_t flags = u16(&cd[pos + 8]), name_len = u16(&cd[pos + 28]), extra_len = u16(&cd[pos + 30]), comment_len = u16(&cd[pos + 32]);
        e.method = u16(&cd[pos + 10]); e.crc = u32(&cd[pos + 16]); e.csize = u32(&cd[pos + 20]); e.usize = u32(&cd[pos + 24]);
        e.offset = u32(&cd[pos + 42]);
        if (pos + 46 + name_len + extra_len + comment_len > cd.size()) { error = "package directory is damaged"; return false; }
        e.name.assign((const char*)&cd[pos + 46], name_len);
        pos += 46 + name_len + extra_len + comment_len;
        if (flags & 1) { error = "encrypted packages are not supported"; return false; }
        if (!safe_entry_name(e.name)) { error = "unsafe path in package: " + e.name; return false; }
        if (e.csize == 0xFFFFFFFFu || e.usize == 0xFFFFFFFFu || e.offset == 0xFFFFFFFFu) { error = "zip64 packages are not supported"; return false; }
        total += e.usize;
        entries.push_back(e);
    }
    if (total > (512ull << 20)) { error = "package is unreasonably large"; return false; }

    if (!make_directories(dest)) { error = "cannot create " + dest; return false; }
    uint64_t done = 0;
    std::vector<unsigned char> in(64 * 1024), out(64 * 1024);
    for (const auto& e : entries) {
        std::string target = dest + "/" + e.name;
        if (e.name.back() == '/') {
            if (!make_directories(target)) { error = "cannot create " + target; return false; }
            continue;
        }
        size_t slash = target.rfind('/');
        if (slash != std::string::npos && !make_directories(target.substr(0, slash))) { error = "cannot create " + target; return false; }
        unsigned char local[30];
        std::fseek(zip, e.offset, SEEK_SET);
        if (std::fread(local, 1, 30, zip) != 30 || u32(local) != 0x04034b50) { error = "package entry is damaged: " + e.name; return false; }
        std::fseek(zip, e.offset + 30 + u16(local + 26) + u16(local + 28), SEEK_SET);
        FILE* file = std::fopen(target.c_str(), "wb");
        if (!file) { error = "cannot write " + target; return false; }
        uint32_t crc = crc32(0, nullptr, 0);
        uint32_t left = e.csize, produced = 0;
        bool ok = true;
        z_stream stream{};
        bool inflating = e.method == 8;
        if (e.method != 0 && e.method != 8) { error = "unsupported compression in " + e.name; ok = false; }
        if (ok && inflating && inflateInit2(&stream, -15) != Z_OK) { error = "zlib init failed"; ok = false; }
        auto emit = [&](const unsigned char* data, size_t n) {
            if (n && std::fwrite(data, 1, n, file) != n) { error = "disk write failed (storage full?)"; return false; }
            crc = crc32(crc, data, (uInt)n);
            produced += (uint32_t)n; done += n;
            if (progress && !progress(done, total)) { error = "cancelled"; return false; }
            return true;
        };
        int zr = Z_OK;
        while (ok && left) {
            size_t chunk = std::min<size_t>(left, in.size());
            if (std::fread(in.data(), 1, chunk, zip) != chunk) { error = "package read failed"; ok = false; break; }
            left -= (uint32_t)chunk;
            if (!inflating) { ok = emit(in.data(), chunk); continue; }
            stream.next_in = in.data(); stream.avail_in = (uInt)chunk;
            do {
                stream.next_out = out.data(); stream.avail_out = (uInt)out.size();
                zr = inflate(&stream, Z_NO_FLUSH);
                if (zr != Z_OK && zr != Z_STREAM_END) { error = "corrupt data in " + e.name; ok = false; break; }
                if (!emit(out.data(), out.size() - stream.avail_out)) { ok = false; break; }
            } while (stream.avail_out == 0 && zr != Z_STREAM_END);
        }
        if (inflating) { if (ok && zr != Z_STREAM_END && e.usize) { error = "truncated data in " + e.name; ok = false; } inflateEnd(&stream); }
        if (std::fclose(file) != 0 && ok) { error = "disk write failed (storage full?)"; ok = false; }
        if (ok && (produced != e.usize || crc != e.crc)) { error = "checksum mismatch in " + e.name; ok = false; }
        if (!ok) return false;
    }
    return true;
}

} // namespace update
