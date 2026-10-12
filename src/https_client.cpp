#include "https_client.h"
#include "update_core.h"
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstring>

#ifndef VITAGRAM_VERSION
#define VITAGRAM_VERSION "dev"
#endif

namespace update {
namespace {

struct Connection {
    int fd = -1;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    ~Connection() {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        if (ctx) SSL_CTX_free(ctx);
        if (fd >= 0) close(fd);
    }
};

std::string ssl_error_text(const char* what) {
    unsigned long code = ERR_get_error();
    char buffer[160] = "";
    if (code) ERR_error_string_n(code, buffer, sizeof(buffer));
    return std::string(what) + (code ? std::string(": ") + buffer : "");
}

bool connect_tls(Connection& c, const Url& url, const std::string& ca_file, std::string& error) {
    static bool initialised = false;
    if (!initialised) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
        SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms();
#endif
        initialised = true;
    }
    addrinfo hints{}, *list = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &list) != 0 || !list) { error = "cannot resolve " + url.host; return false; }
    for (addrinfo* a = list; a && c.fd < 0; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        timeval timeout{10, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) c.fd = fd; else close(fd);
    }
    freeaddrinfo(list);
    if (c.fd < 0) { error = "cannot connect to " + url.host; return false; }

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    c.ctx = SSL_CTX_new(SSLv23_client_method());
#else
    c.ctx = SSL_CTX_new(TLS_client_method());
#endif
    if (!c.ctx) { error = ssl_error_text("TLS init failed"); return false; }
    SSL_CTX_set_options(c.ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
    if (!load_ca_bundle(SSL_CTX_get_cert_store(c.ctx), ca_file, error)) return false;
    SSL_CTX_set_verify(c.ctx, SSL_VERIFY_PEER, nullptr);
    c.ssl = SSL_new(c.ctx);
    if (!c.ssl) { error = ssl_error_text("TLS init failed"); return false; }
    SSL_set_fd(c.ssl, c.fd);
    SSL_set_tlsext_host_name(c.ssl, url.host.c_str());
    X509_VERIFY_PARAM_set1_host(SSL_get0_param(c.ssl), url.host.c_str(), 0);
    if (SSL_connect(c.ssl) != 1) {
        long verify = SSL_get_verify_result(c.ssl);
        error = verify != X509_V_OK ? std::string("certificate rejected: ") + X509_verify_cert_error_string(verify)
                                    : ssl_error_text("TLS handshake failed");
        return false;
    }
    return true;
}

} // namespace

HttpsResult https_get(const std::string& start_url, const std::string& ca_file, const std::string& accept,
                      const std::function<bool(const char*, size_t)>& sink, const std::atomic<bool>* cancel,
                      const std::function<void(int64_t)>& on_length) {
    HttpsResult result;
    std::string current = start_url;
    for (int hop = 0; hop <= 5; ++hop) {
        Url url;
        if (!split_https_url(current, url)) { result.error = "refusing a non-https address"; return result; }
        Connection conn;
        if (!connect_tls(conn, url, ca_file, result.error)) return result;
        std::string request = "GET " + url.path + " HTTP/1.1\r\nHost: " + url.host +
            "\r\nUser-Agent: VitaGram/" VITAGRAM_VERSION "\r\nAccept: " + accept +
            "\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
        if (SSL_write(conn.ssl, request.data(), (int)request.size()) != (int)request.size()) { result.error = ssl_error_text("request failed"); return result; }

        HttpResponse response;
        bool length_reported = false, sink_failed = false;
        auto body = [&](const char* data, size_t size) {
            if (response.status() < 200 || response.status() >= 300) return true; // error pages are dropped
            if (!sink(data, size)) { sink_failed = true; return false; }
            return true;
        };
        char buffer[16384];
        while (!response.complete()) {
            if (cancel && cancel->load()) { result.error = "cancelled"; return result; }
            int n = SSL_read(conn.ssl, buffer, sizeof(buffer));
            if (n <= 0) {
                int err = SSL_get_error(conn.ssl, n);
                if (err == SSL_ERROR_ZERO_RETURN || (err == SSL_ERROR_SYSCALL && n == 0)) break; // peer closed
                result.error = ssl_error_text("download interrupted");
                return result;
            }
            if (!response.feed(buffer, (size_t)n, body)) {
                result.error = sink_failed ? "cancelled" : "malformed server response";
                return result;
            }
            if (response.headersDone() && !length_reported) {
                length_reported = true;
                if (on_length && response.status() >= 200 && response.status() < 300) on_length(response.contentLength());
            }
        }
        result.status = response.status();
        result.content_length = response.contentLength();
        if (!response.headersDone()) { result.error = "connection closed before a response"; return result; }
        int s = response.status();
        if (s == 301 || s == 302 || s == 303 || s == 307 || s == 308) {
            std::string next;
            if (!resolve_location(url, response.location(), next)) { result.error = "refusing a redirect away from https"; return result; }
            current = next;
            continue;
        }
        if (s < 200 || s >= 300) { result.error = "server answered HTTP " + std::to_string(s); return result; }
        // Truncation: a length-framed or chunked body must have arrived completely.
        if (!response.complete() && !response.bodyIsUntilClose()) { result.error = "download was cut off"; return result; }
        result.error.clear();
        return result;
    }
    result.error = "too many redirects";
    return result;
}

} // namespace update
