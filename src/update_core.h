#pragma once
// Platform-independent parts of the self-updater: version handling, GitHub release parsing,
// HTTP/1.1 response framing, release signature check and VPK (zip) extraction.
// Everything here builds and is tested on the host (tests/run-update-tests.sh).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace update {

// "v0.35.4", "0.35.4" or "0.36" -> {0,35,4}; false when not a plain dotted number.
bool parse_version(const std::string& text, int out[3]);
std::string normalize_version(const std::string& text); // without the leading 'v'
// -1 / 0 / 1; an unparsable side compares as "not newer" (0).
int compare_versions(const std::string& a, const std::string& b);

struct Release {
    std::string version, vpk_name, vpk_url, sig_url;
    uint64_t vpk_size = 0;
};
// GitHub "latest release" JSON. Drafts and pre-releases are rejected; a release without
// a .vpk asset and its detached .vpk.sig signature is rejected too (unsigned = unusable).
bool parse_release(const std::string& json, Release& out, std::string& error);

struct Url { std::string host, port, path; };
bool split_https_url(const std::string& url, Url& out); // only https://host[:port]/path
// Absolute https URL or "/path" relative to `base`.
bool resolve_location(const Url& base, const std::string& location, std::string& absolute);

// Incremental HTTP/1.1 response reader (Content-Length, chunked or read-until-close).
class HttpResponse {
public:
    using Sink = std::function<bool(const char*, size_t)>;
    // false: malformed response or the sink aborted.
    bool feed(const char* data, size_t size, const Sink& sink);
    bool headersDone() const { return state_ != State::Headers; }
    bool complete() const { return state_ == State::Done; }
    int status() const { return status_; }
    const std::string& location() const { return location_; }
    int64_t contentLength() const { return content_length_; }
    uint64_t bodyBytes() const { return body_bytes_; }
    bool bodyIsUntilClose() const { return !chunked_ && content_length_ < 0; }
private:
    enum class State { Headers, Body, ChunkSize, ChunkData, ChunkDataEnd, Trailer, Done };
    bool parseHeaders();
    State state_ = State::Headers;
    std::string buffer_;
    int status_ = 0;
    std::string location_;
    int64_t content_length_ = -1;
    bool chunked_ = false;
    uint64_t body_bytes_ = 0, chunk_left_ = 0;
};

// Release signature: ECDSA P-256 / SHA-256 over "vitagram-update-v1\n<version>\n" + VPK bytes.
// Binding the version stops a stale (but genuinely signed) VPK from passing as a newer release.
class SignatureVerifier {
public:
    SignatureVerifier();
    ~SignatureVerifier();
    SignatureVerifier(const SignatureVerifier&) = delete;
    SignatureVerifier& operator=(const SignatureVerifier&) = delete;
    bool begin(const std::string& version, const char* public_key_pem = nullptr); // null = built-in key
    void update(const void* data, size_t size);
    bool finish(const unsigned char* signature, size_t size);
private:
    struct Impl;
    Impl* impl_;
};

// Extracts a zip (stored/deflate, no zip64) below `dest`. Entries with absolute paths, "..",
// backslashes or drive letters are refused. `progress` gets (done, total) uncompressed bytes.
bool extract_zip(const std::string& zip_path, const std::string& dest,
                 const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error);
bool safe_entry_name(const std::string& name);
bool make_directories(const std::string& path);
bool remove_tree(const std::string& path);

} // namespace update
