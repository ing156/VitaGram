#include "secure_store.h"
#include <cassert>
#include <iostream>

int main() {
    std::string plain;
    if (!secure_store::available()) {
        assert(secure_store::protect("secret") == "secret");
        assert(secure_store::database_key().empty());
        assert(secure_store::reveal("legacy", plain) && plain == "legacy");
        std::cout << "Secure store without console id: plain-text fallback PASS\n";
        return 0;
    }
    std::string a = secure_store::protect("s3cret-p@ss"), b = secure_store::protect("s3cret-p@ss");
    assert(secure_store::is_protected(a) && a.find("s3cret") == std::string::npos);
    assert(a != b); // random IV
    assert(secure_store::reveal(a, plain) && plain == "s3cret-p@ss");
    assert(secure_store::protect(a) == a);          // already protected
    assert(secure_store::protect("").empty());
    assert(secure_store::reveal("plain", plain) && plain == "plain"); // legacy migration
    std::string tampered = a; tampered.back() = tampered.back() == '0' ? '1' : '0';
    assert(!secure_store::reveal(tampered, plain) && plain.empty());
    assert(!secure_store::reveal("enc1:zz", plain) && !secure_store::reveal("enc1:", plain));
    assert(secure_store::database_key().size() == 64);
    assert(secure_store::database_key() == secure_store::database_key());
    std::cout << "Secure store: round trip, tamper detection, legacy and key derivation PASS\n";
}
