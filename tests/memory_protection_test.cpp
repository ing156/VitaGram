// Protection of the running session: heap wiping policy, crash-dump removal, injected-module
// detection and the masked in-RAM vault keys.
#include "crash_dump.h"
#include "memory_hygiene.h"
#include "process_modules.h"
#include "vault.h"
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

static bool exists(const std::string& path) { struct stat info{}; return stat(path.c_str(), &info) == 0; }

static void write_gz(const std::string& path, const std::string& data) {
    gzFile f = gzopen(path.c_str(), "wb");
    assert(f && gzwrite(f, data.data(), (unsigned)data.size()) == (int)data.size());
    gzclose(f);
}

// Shaped like a real dump: ELF header, notes, the main thread named after the title, then memory.
static std::string fake_core(const char* thread_name) {
    std::string core("\x7f" "ELF", 4);
    core.append(2000, '\0');
    core.append("THREAD_INFO", 12);
    core.append(16, '\x01');
    core.append(thread_name, std::strlen(thread_name) + 1);
    core.append(300000, 'm');
    return core;
}

static void heap_policy() {
    using namespace memory_hygiene;
    assert(wipe_length(64, true) == 64);
    assert(wipe_length(kMaxWipe, true) == kMaxWipe);
    assert(wipe_length(kMaxWipe + 1, true) == 0); // media-sized blocks are released as they are
    assert(wipe_length(64, false) == 0);
    set_enabled(true); assert(enabled());
    set_enabled(false); assert(!enabled());
    char secret[32];
    std::memset(secret, 'k', sizeof(secret));
    clear(secret, sizeof(secret));
    for (char c : secret) assert(c == 0);
    clear(nullptr, 10); // tolerated
}

static void dumps(const std::string& dir) {
    using namespace crash_dump;
    assert(is_dump_name("psp2core-1791641441-0x0002112c57-eboot.bin.psp2dmp"));
    assert(is_dump_name("psp2core-1791641441-0x0002112c57-eboot.bin.psp2dmp.tmp"));
    assert(!is_dump_name("psp2core-notes.txt") && !is_dump_name("core-1.psp2dmp") && !is_dump_name("vitagram"));

    const std::string ours = dir + "/psp2core-1-0x01-eboot.bin.psp2dmp";
    const std::string ours_partial = dir + "/psp2core-2-0x02-eboot.bin.psp2dmp.tmp";
    const std::string other = dir + "/psp2core-3-0x03-eboot.bin.psp2dmp";
    const std::string prefix_only = dir + "/psp2core-4-0x04-eboot.bin.psp2dmp";
    const std::string unrelated = dir + "/notes.psp2dmp.txt";
    write_gz(ours, fake_core("VITAGRM01"));
    write_gz(ours_partial, fake_core("VITAGRM01").substr(0, 4000));
    write_gz(other, fake_core("VITASHELL"));
    write_gz(prefix_only, fake_core("VITAGRM012")); // another title whose id merely starts the same
    write_gz(unrelated, fake_core("VITAGRM01"));
    assert(belongs_to(ours, "VITAGRM01") && belongs_to(ours_partial, "VITAGRM01"));
    assert(!belongs_to(other, "VITAGRM01") && !belongs_to(prefix_only, "VITAGRM01"));
    assert(!belongs_to(dir + "/missing.psp2dmp", "VITAGRM01"));

    assert(purge(dir, "VITAGRM01") == 2);
    assert(!exists(ours) && !exists(ours_partial));
    assert(exists(other) && exists(prefix_only) && exists(unrelated));
    assert(purge(dir, "VITAGRM01") == 0);
    assert(purge(dir + "/no-such-directory", "VITAGRM01") == 0);
}

static void modules() {
    using process_modules::foreign_path;
    const std::string id = process_modules::kTitleId;
    assert(!foreign_path("vs0:sys/external/libpgf.suprx", id));
    assert(!foreign_path("os0:us/libkernel.suprx", id));
    assert(!foreign_path("app0:eboot.bin", id) && !foreign_path("app0:sce_module/libc.suprx", id));
    assert(!foreign_path("ux0:app/VITAGRM01/eboot.bin", id) && !foreign_path("UX0:APP/vitagrm01/eboot.bin", id));
    assert(!foreign_path("", id));
    assert(!foreign_path("ux0:/app/VITAGRM01/eboot.bin", id) && !foreign_path("ux0:app//VITAGRM01/eboot.bin", id));
    assert(!foreign_path("uma0:/app/VITAGRM01/sce_module/libc.suprx", id) && !foreign_path("app0:/eboot.bin", id));
    assert(foreign_path("ux0:/tai/remapsv2.suprx", id));
    assert(foreign_path("ux0:tai/remapsv2.suprx", id));
    assert(foreign_path("ur0:tai/keylogger.suprx", id));
    assert(foreign_path("uma0:plugins/x.suprx", id));
    assert(foreign_path("ux0:app/VITAGRM012/eboot.bin", id)); // another title's directory
    assert(foreign_path("ux0:app/OTHER0001/sce_module/x.suprx", id));
    assert(process_modules::foreign(id).empty()); // host build: nothing to enumerate
    assert(process_modules::summary({}) == "");
    assert(process_modules::summary({"a.suprx"}) == "a.suprx");
    assert(process_modules::summary({"a", "b", "c", "d", "e"}) == "a, b, c +2");
}

static void masked_keys(const std::string& dir) {
    using namespace vault;
    set_config_path(dir + "/vault.json");
    load();
    const Pin pin{0, 4, 2, 6, 7, 3, 1, 5, 4, 0, 6, 2};
    assert(enable(pin) == Result::Ok);
    const std::string db_key = database_key();
    assert(db_key.size() == 64 && db_key.find_first_not_of("0123456789abcdef") == std::string::npos);
    uint8_t file_a[32], file_b[32];
    assert(file_key(file_a));
    assert(write_file_atomic(dir + "/sealed", "session data") && is_vault_sealed(dir + "/sealed"));
    set_migration_pending(false); // signed with the masked state key

    // A new session re-randomizes the pad; the keys it yields must not change.
    lock();
    assert(database_key().empty() && !file_key(file_b));
    load();
    assert(unlock(pin) == Result::Ok);
    assert(!state_unauthenticated() && !migration_pending()); // the signature made with the masked key verifies
    assert(database_key() == db_key);
    assert(file_key(file_b) && !std::memcmp(file_a, file_b, 32));
    std::string back;
    assert(read_file(dir + "/sealed", 1 << 10, back) && back == "session data");
    lock();
    assert(database_key().empty());

    // Pin is still an ordinary byte sequence for the vault API.
    Pin grown;
    for (int i = 0; i < 100; ++i) grown.push_back((uint8_t)(i % 8));
    assert(grown.size() == 100 && grown[9] == 1);
}

int main() {
    char tmpl[] = "/tmp/vgmemoryXXXXXX";
    const std::string dir = mkdtemp(tmpl);
    heap_policy();
    dumps(dir);
    modules();
    masked_keys(dir);
    if (std::system(("rm -rf '" + dir + "'").c_str()) != 0) std::cerr << "could not remove " << dir << "\n";
    std::cout << "memory protection tests passed\n";
}
