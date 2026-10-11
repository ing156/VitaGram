#include "vault.h"
#include <jansson.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>

using namespace vault;

static std::string dir;
static std::string slurp(const std::string& p) {
    std::string s; FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return s;
    char b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f))) s.append(b, n); std::fclose(f); return s;
}
static void spit(const std::string& p, const std::string& s) { FILE* f = std::fopen(p.c_str(), "wb"); std::fwrite(s.data(), 1, s.size(), f); std::fclose(f); }
// Edits vault.json the way someone with the memory card could: fields only, the wrapped key untouched.
static void edit_config(const std::string& path, const char* mode, int migrating, bool drop_signature) {
    json_t* root = json_load_file(path.c_str(), 0, nullptr);
    assert(root);
    if (mode) json_object_set_new(root, "mode", json_string(mode));
    if (migrating >= 0) json_object_set_new(root, "migrating", json_boolean(migrating));
    if (drop_signature) json_object_del(root, "state");
    assert(json_dump_file(root, path.c_str(), JSON_COMPACT) == 0);
    json_decref(root);
}
static std::string random_data(size_t n, uint64_t seed) { std::mt19937_64 r(seed); std::string s(n, 0); for (auto& c : s) c = (char)r(); return s; }

int main() {
    char tmpl[] = "/tmp/vgvaultXXXXXX";
    dir = mkdtemp(tmpl);
    set_config_path(dir + "/vault.json");
    load();
    assert(state() == State::Undecided && !enabled() && !sealing());

    // Plain files stay readable and writable without a vault.
    spit(dir + "/plain", "hello");
    { Reader r; assert(r.open(dir + "/plain") && !r.sealed() && r.size() == 5); char b[8]; assert(r.read(b, 8) == 5 && !std::memcmp(b, "hello", 5)); }

    // Caches without a vault are sealed with the console key: never plain on the card, readable
    // without a vault, and not mistaken for vault-sealed data.
    const std::string cache_data = random_data(150000, 7);
    {
        Writer w; assert(w.open_cache(dir + "/cache.json") && w.write(cache_data.data(), cache_data.size()) && w.close());
        assert(is_sealed(dir + "/cache.json") && !is_vault_sealed(dir + "/cache.json"));
        assert(slurp(dir + "/cache.json").find(cache_data.substr(0, 64)) == std::string::npos);
        Reader r; assert(r.open(dir + "/cache.json") && r.sealed() && r.console_sealed() && r.size() == (int64_t)cache_data.size());
        std::string back; assert(read_file(dir + "/cache.json", 1 << 20, back) && back == cache_data);
        assert(seal_file(dir + "/cache.json") == Convert::Failed); // no vault: nothing to re-seal with
    }

    Pin weak{1, 1, 1, 1, 1, 1, 1, 1}, pin{0, 4, 2, 6, 7, 3, 1, 5, 4, 0, 6, 2}, other{0, 4, 2, 6, 7, 3, 1, 5, 4, 0, 6, 3};
    assert(enable(Pin{0, 1, 2}) == Result::TooShort && enable(weak) == Result::TooShort);
    assert(enable(Pin{0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1}) == Result::TooShort);
    assert(!pin_acceptable(Pin{0, 1, 2, 3, 4, 5, 6, 9}));
    assert(enable(pin) == Result::Ok && unlocked() && sealing() && enabled());
    // Turning the vault on re-seals console-key caches under the vault key.
    assert(seal_file(dir + "/cache.json") == Convert::Done && is_vault_sealed(dir + "/cache.json"));
    assert(seal_file(dir + "/cache.json") == Convert::Unchanged);
    { std::string back; assert(read_file(dir + "/cache.json", 1 << 20, back) && back == cache_data); }
    { Writer w; assert(w.open_cache(dir + "/cache2.json") && w.write("x", 1) && w.close() && is_vault_sealed(dir + "/cache2.json")); }
    std::string config = slurp(dir + "/vault.json");
    assert(config.find("wrapped") != std::string::npos);
    assert(database_key().size() == 64);
    std::string db_key = database_key();

    // Round trips around every chunk boundary, plus random access.
    for (size_t n : {0u, 1u, 15u, 16u, 65535u, 65536u, 65537u, 131072u, 200001u}) {
        std::string data = random_data(n, n + 1), path = dir + "/f" + std::to_string(n);
        Writer w; assert(w.open(path) && w.write(data.data(), n / 3) && w.write(data.data() + n / 3, n - n / 3) && w.close());
        assert(is_sealed(path));
        struct stat st{}; stat(path.c_str(), &st);
        assert(st.st_size == sealed_size((int64_t)n) && plain_size_from_sealed(st.st_size) == (int64_t)n && plain_size(path) == (int64_t)n);
        if (n > 16) assert(slurp(path).find(data.substr(0, 16)) == std::string::npos);
        std::string back; assert(read_file(path, 1 << 20, back) && back == data);
        Reader r; assert(r.open(path) && r.sealed() && r.size() == (int64_t)n);
        std::mt19937 rng(7);
        for (int i = 0; i < 20 && n; ++i) {
            size_t off = rng() % n, len = 1 + rng() % 70000; char buf[70001];
            assert(r.seek((int64_t)off, SEEK_SET) == (int64_t)off);
            int64_t got = r.read(buf, len);
            assert(got == (int64_t)std::min(len, n - off) && !std::memcmp(buf, data.data() + off, (size_t)got));
        }
        assert(r.seek(0, SEEK_END) == (int64_t)n && r.read(nullptr, 1) == 0);
    }
    assert(sealed_size(0) == 24 + 16 && plain_size_from_sealed(24 + 16) == 0 && plain_size_from_sealed(24 + 17) == 1);
    assert(plain_size_from_sealed(24 + 15) == -1 && plain_size_from_sealed(24 + 65536 + 16) == 65536 && plain_size_from_sealed(24 + 65536 + 17) == -1);

    // Two files with identical content must not share ciphertext (random per-file salt).
    { std::string a = random_data(5000, 9); Writer w1, w2; assert(w1.open(dir + "/a1") && w2.open(dir + "/a2")); w1.write(a.data(), a.size()); w2.write(a.data(), a.size()); assert(w1.close() && w2.close()); assert(slurp(dir + "/a1") != slurp(dir + "/a2")); }

    // Tampering: bit flip, truncation, extension, chunk swap, header swap between files, wrong file key.
    {
        std::string data = random_data(3 * 65536 + 100, 11), path = dir + "/t";
        Writer w; assert(w.open(path) && w.write(data.data(), data.size()) && w.close());
        std::string sealed = slurp(path), back;
        auto reject = [&](const std::string& mutated) { spit(dir + "/m", mutated); std::string out; return !read_file(dir + "/m", 1 << 22, out); };
        auto flip = sealed; flip[24 + 100] ^= 1; assert(reject(flip));
        auto flip_tag = sealed; flip_tag[24 + 65536 + 3] ^= 1; assert(reject(flip_tag));
        auto flip_header = sealed; flip_header[10] ^= 1; assert(reject(flip_header));
        const size_t stride = 65536 + 16;
        assert(reject(sealed.substr(0, 24 + 2 * stride)));                         // cut at a chunk boundary
        assert(reject(sealed + std::string(32, 'x')));                            // appended junk
        assert(reject(sealed.substr(0, sealed.size() - 1)));
        auto swapped = sealed; swapped.replace(24, stride, sealed.substr(24 + stride, stride)); swapped.replace(24 + stride, stride, sealed.substr(24, stride)); assert(reject(swapped));
        std::string other_file = random_data(3 * 65536 + 100, 12); Writer w2; assert(w2.open(dir + "/t2") && w2.write(other_file.data(), other_file.size()) && w2.close());
        auto grafted = sealed; grafted.replace(24, stride, slurp(dir + "/t2").substr(24, stride)); assert(reject(grafted));
        assert(read_file(path, 1 << 22, back) && back == data);
    }

    // Convert existing plain files in place, both ways.
    {
        std::string data = random_data(150000, 21); spit(dir + "/c", data);
        assert(seal_file(dir + "/c") == Convert::Done && is_sealed(dir + "/c") && seal_file(dir + "/c") == Convert::Unchanged);
        std::string back; assert(read_file(dir + "/c", 1 << 20, back) && back == data);
        assert(unseal_file(dir + "/c") == Convert::Done && !is_sealed(dir + "/c") && slurp(dir + "/c") == data && unseal_file(dir + "/c") == Convert::Unchanged);
        spit(dir + "/empty", ""); assert(seal_file(dir + "/empty") == Convert::Done && plain_size(dir + "/empty") == 0);
        assert(access((dir + "/c.vgs").c_str(), F_OK) != 0);
        assert(write_file_atomic(dir + "/j", "{\"a\":1}") && is_sealed(dir + "/j"));
        assert(read_file(dir + "/j", 100, back) && back == "{\"a\":1}");
    }

    // Persisted vault: wrong key, right key, key change.
    lock();
    assert(!unlocked() && !sealing() && database_key().empty());
    { Reader r; assert(!r.open(dir + "/f65536")); }                                // sealed data needs the key
    load();
    assert(state() == State::Locked && enabled());
    assert(unlock(other) == Result::WrongPin && !unlocked());
    assert(unlock(pin) == Result::Ok && unlocked() && database_key() == db_key);
    std::string back; assert(read_file(dir + "/f65537", 1 << 20, back) && back == random_data(65537, 65538));
    assert(change_pin(other, pin) == Result::WrongPin);
    assert(change_pin(pin, other) == Result::Ok && database_key() == db_key);       // same master key, new wrapping
    lock(); load();
    assert(unlock(pin) == Result::WrongPin && unlock(other) == Result::Ok && database_key() == db_key);
    // Tampered or damaged configuration must never unlock.
    lock();
    { std::string c = slurp(dir + "/vault.json"); auto at = c.find("\"n\":14"); assert(at != std::string::npos); c.replace(at, 6, "\"n\":10"); spit(dir + "/vault.json", c); load(); assert(unlock(other) != Result::Ok); }
    spit(dir + "/vault.json", "{broken"); load(); assert(state() == State::Locked && unlock(other) == Result::Error);
    spit(dir + "/vault.json", config); load(); assert(unlock(pin) == Result::Ok);

    // Repeated wrong keys trigger a lockout.
    lock(); load();
    for (int i = 0; i < 5; ++i) assert(unlock(other) == Result::WrongPin);
    assert(lockout_seconds() > 0 && unlock(pin) == Result::WrongPin);
    reset(); load(); assert(state() == State::Undecided);

    // Switching off.
    set_config_path(dir + "/vault2.json"); load();
    assert(enable(pin) == Result::Ok);
    std::string data = random_data(70000, 5); Writer w; assert(w.open(dir + "/d") && w.write(data.data(), data.size()) && w.close());
    lock(); load();
    assert(begin_disable(other) == Result::WrongPin && begin_disable(pin) == Result::Ok);
    assert(unlocked() && !sealing() && disabling() && enabled());
    { Writer plain; assert(plain.open(dir + "/new") && plain.write("x", 1) && plain.close() && !is_sealed(dir + "/new")); }
    assert(unseal_file(dir + "/d") == Convert::Done && slurp(dir + "/d") == data);
    lock(); load(); assert(disabling() && state() == State::Locked);   // interrupted: needs the key again
    assert(unlock(pin) == Result::Ok && !sealing());
    assert(!finish_disable() && enabled());          // files may still be sealed: the key is kept
    set_migration_pending(false);
    assert(finish_disable() && !enabled() && state() == State::Off && !unlocked());
    load(); assert(state() == State::Off && !enabled());
    { Writer after; assert(after.open(dir + "/after") && !is_sealed(dir + "/after")); after.abort(); }
    reset(); load(); assert(decline() && state() == State::Off);

    // The security state is signed: an edited "disabling" does not survive the owner's unlock (F01).
    {
        const std::string cfg = dir + "/vault5.json";
        set_config_path(cfg); load();
        assert(enable(pin) == Result::Ok);
        set_migration_pending(false);
        assert(slurp(cfg).find("\"state\":\"") != std::string::npos && !migration_pending());
        lock(); edit_config(cfg, "disabling", 1, false); load();
        assert(disabling() && state() == State::Locked);
        assert(unlock(pin) == Result::Ok && state_was_reset());
        assert(!disabling() && sealing() && migration_pending()); // stays on, re-checks every file
        // The reset state is signed again: the next unlock is quiet.
        lock(); load(); assert(unlock(pin) == Result::Ok && !state_was_reset() && !disabling() && sealing());
        // Marking an unfinished migration as done is undone too (wrong signature).
        lock(); edit_config(cfg, nullptr, 0, false); load();
        assert(!migration_pending() && unlock(pin) == Result::Ok && migration_pending() && sealing());
        // A file from an older version (no signature) in the ordinary "on" state: re-checked quietly.
        set_migration_pending(false);
        lock(); edit_config(cfg, nullptr, -1, true); load();
        assert(unlock(pin) == Result::Ok && !state_was_reset() && migration_pending() && sealing());
        // No signature and "disabling" (edited, or an old version mid-switch-off): reset, and the user is told.
        lock(); edit_config(cfg, "disabling", 1, true); load();
        assert(unlock(pin) == Result::Ok && state_was_reset() && !disabling() && sealing());
        // A locked vault cannot sign, so it does not change the flag at all.
        lock(); load(); set_migration_pending(false); load(); assert(migration_pending());
        reset();
    }

    // Passphrase method.
    set_config_path(dir + "/vault3.json"); load();
    Pin phrase{'c','o','r','r','e','c','t',' ','h','o','r','s','e',' ','9','!'}, short_phrase{'a','b','c'};
    assert(method() == Method::Buttons);
    assert(enable(short_phrase, Method::Text) == Result::TooShort);
    assert(enable(Pin(12, 'a'), Method::Text) == Result::TooShort);                      // one repeated character
    assert(!secret_acceptable(Pin{'a','b','a','b','a','b','a','b','a','b','a','b'}, Method::Text));
    assert(secret_strength_bits(phrase, Method::Text) > secret_strength_bits(Pin{'a','b','c','d','e','f','g','h','i','j'}, Method::Text));
    assert(enable(phrase, Method::Text) == Result::Ok && method() == Method::Text && database_key().size() == 64);
    std::string text_db_key = database_key();
    assert(slurp(dir + "/vault3.json").find("\"method\":\"text\"") != std::string::npos);
    lock(); load();
    assert(method() == Method::Text && state() == State::Locked);
    Pin wrong_phrase = phrase; wrong_phrase.back() = '?';
    assert(unlock(wrong_phrase) == Result::WrongPin && unlock(phrase) == Result::Ok && database_key() == text_db_key);
    // The same bytes entered as a button key do not open a text vault, and a text vault accepts bytes above 7.
    lock(); load(); assert(unlock(Pin{0, 1, 2, 3, 4, 5, 6, 7}) == Result::WrongPin);
    // Switching method keeps the master key.
    reset(); lock();
    set_config_path(dir + "/vault4.json"); load();
    assert(enable(pin, Method::Buttons) == Result::Ok);
    std::string switch_key = database_key();
    assert(change_pin(pin, phrase, Method::Text) == Result::Ok && method() == Method::Text && database_key() == switch_key);
    lock(); load(); assert(unlock(phrase) == Result::Ok && database_key() == switch_key);
    assert(change_pin(phrase, pin, Method::Buttons) == Result::Ok);
    lock(); load(); assert(method() == Method::Buttons && unlock(pin) == Result::Ok && database_key() == switch_key);

    std::string cmd = "rm -rf " + dir; (void)!system(cmd.c_str());
    std::cout << "Vault: key hierarchy, sealed containers, random access, tamper rejection, conversion, lockout PASS\n";
}
