#include "test_language.h"
#include "proxy_link.h"
#include <cassert>
#include <iostream>

int main() {
    use_test_language();
    ProxyLink proxy; std::string error;
    const std::string secret = "0123456789abcdef0123456789abcdef";
    auto valid = [&](const std::string& url, const std::string& server, int port, const std::string& value) {
        assert(parse_proxy_link(url, proxy, error) == ProxyLinkResult::Valid);
        assert(error.empty() && proxy.server == server && proxy.port == port && proxy.secret == value);
    };
    valid("tg://proxy?server=proxy.example&port=443&secret=" + secret, "proxy.example", 443, secret);
    valid("TG://PROXY/?secret=" + secret + "&port=65535&server=127.0.0.1#ignored", "127.0.0.1", 65535, secret);
    valid("https://t.me/proxy?server=%5B2001%3Adb8%3A%3A1%5D&port=1&secret=" + secret, "[2001:db8::1]", 1, secret);
    valid("tg:proxy?server=host&port=80&secret=A%2BB%2FC%3D", "host", 80, "A+B/C=");
    valid("t.me/proxy?server=host&port=80&secret=A+B/C=", "host", 80, "A+B/C=");
    for (const char* prefix : {"http://telegram.me/proxy?", "https://telegram.dog/proxy?"})
        valid(std::string(prefix) + "server=host&port=80&secret=" + secret, "host", 80, secret);
    for (const char* url : {
        "tg://proxy", "tg://proxy?server=host&port=443", "tg://proxy?server=&port=443&secret=a",
        "tg://proxy?server=host&port=0&secret=a", "tg://proxy?server=host&port=65536&secret=a",
        "tg://proxy?server=host&port=-1&secret=a", "tg://proxy?server=host&port=1a&secret=a",
        "tg://proxy?server=host&port=999999999999999999&secret=a",
        "tg://proxy?server=host&port=1&port=2&secret=a", "tg://proxy?server=host&server=other&port=1&secret=a",
        "tg://proxy?server=host&port=1&secret=a&secret=b", "tg://proxy?server=host%00evil&port=1&secret=a",
        "tg://proxy?server=host%0Aevil&port=1&secret=a", "tg://proxy?server=host%ZZ&port=1&secret=a",
        "tg://proxy?server=host%&port=1&secret=a", "tg://proxy?server=https://host&port=1&secret=a",
        "tg://proxy#fragment?server=host&port=1&secret=a"}) {
        ProxyLink unchanged = proxy;
        assert(parse_proxy_link(url, proxy, error) == ProxyLinkResult::Invalid);
        assert(!error.empty());
        assert(proxy.server == unchanged.server && proxy.port == unchanged.port && proxy.secret == unchanged.secret);
    }
    for (const char* url : {"https://t.me.evil/proxy?server=host", "https://t.me@evil/proxy?server=host",
                           "tg://proxy.evil?server=host", "https://example.org/"})
        assert(parse_proxy_link(url, proxy, error) == ProxyLinkResult::NotProxy);
    // SOCKS5 links: credentials optional, no secret.
    assert(parse_proxy_link("tg://socks?server=s.example&port=1080&user=bob&pass=p%40ss", proxy, error) == ProxyLinkResult::Valid);
    assert(proxy.kind == ProxyKind::Socks5 && proxy.server == "s.example" && proxy.port == 1080 &&
           proxy.username == "bob" && proxy.password == "p@ss" && proxy.secret.empty());
    assert(parse_proxy_link("https://t.me/socks?server=h&port=1", proxy, error) == ProxyLinkResult::Valid);
    assert(proxy.kind == ProxyKind::Socks5 && proxy.username.empty());
    assert(parse_proxy_link("tg://proxy?server=h&port=1&secret=ab", proxy, error) == ProxyLinkResult::Valid);
    assert(proxy.kind == ProxyKind::MTProto);
    for (const char* url : {"tg://socks?server=host", "tg://socks?server=h&port=1&user=a&user=b",
                            "tg://socks?server=h&port=0", "tg://socks"})
        assert(parse_proxy_link(url, proxy, error) == ProxyLinkResult::Invalid);
    assert(parse_proxy_link("tg://proxy?server=host&port=1&secret=" + std::string(5000, 'a'), proxy, error) == ProxyLinkResult::Invalid);
    std::cout << "Proxy links: PASS\n";
}
