#include "cpu_scheduler.h"
#include "vault.h"
#include "vault_sweep.h"
#include "settings.h"
#include "download_stream.h"
#include <jansson.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

// Stubs for the Vita-only scheduler and kernel calls.
int sceKernelDelayThread(unsigned us) { return usleep(us); }
namespace cpu_scheduler {
Thread::Thread(Role) : id_(0) {}
Thread::~Thread() {}
bool videoActive() { return false; }
}

static std::string slurp(const std::string& p) {
    std::string s; FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return s;
    char b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f))) s.append(b, n); std::fclose(f); return s;
}
static void spit(const std::string& p, const std::string& s) { FILE* f = std::fopen(p.c_str(), "wb"); assert(f); std::fwrite(s.data(), 1, s.size(), f); std::fclose(f); }
static std::string blob(size_t n, char seed) { std::string s(n, 0); for (size_t i = 0; i < n; ++i) s[i] = (char)(seed + i * 7 + (i >> 8)); return s; }

int main() {
    char tmpl[] = "/tmp/vgsweepXXXXXX";
    std::string dir = mkdtemp(tmpl);
    assert(!chdir(dir.c_str()));
    // The Vita paths ("ux0:data/...") are plain relative directory names on the host.
    for (const char* d : {"ux0:data", "ux0:data/vitagram", "ux0:data/vitagram/files", "ux0:data/vitagram/files/photos",
                          "ux0:data/vitagram/files/videos", "ux0:data/vitagram/db", "ux0:data/vitagram/db/stickers",
                          "ux0:data/vitagram/db/thumbnails", "ux0:data/vitagram/db/thumbnails/b1", "ux0:data/vitagram/db/old.trash"})
        assert(!mkdir(d, 0777));
    const std::string files = "ux0:data/vitagram/files", db = "ux0:data/vitagram/db";
    // All theme IDs survive JSON and the pre-unlock hint, including VHBB (ID 2).
    {
        for (int theme = 0; theme < AppSettings::kThemeChoices; ++theme) {
            AppSettings settings; settings.theme = theme; assert(settings.save());
            AppSettings loaded; loaded.load();
            assert(loaded.loaded && loaded.theme == theme);
            assert(loaded.isDark() == (theme != 0));
            assert(AppSettings::themeHint(-1) == theme);
        }
        spit(AppSettings::kThemeHint, "9");
        assert(AppSettings::themeHint(1) == 1);
        spit("ux0:data/vitagram/settings.json", "{}");
        AppSettings defaults; defaults.load(); assert(defaults.theme == 1);
    }
    // Shortcut preferences and the single spoiler mode survive saving; invalid modes use the default.
    {
        AppSettings settings;
        assert(settings.recording_shortcuts && settings.spoiler_reset == 3);
        settings.recording_shortcuts = false;
        for (int mode = 0; mode < AppSettings::kSpoilerResetChoices; ++mode) {
            settings.spoiler_reset = mode; assert(settings.save());
            AppSettings loaded; loaded.load();
            assert(loaded.loaded && !loaded.recording_shortcuts && loaded.spoiler_reset == mode);
            assert(loaded.spoilerHideChat() == bool(mode & 1) && loaded.spoilerHideApp() == bool(mode & 2));
        }
        spit("ux0:data/vitagram/settings.json", R"({"recording_shortcuts":"invalid","spoiler_reset":-1})");
        AppSettings loaded; loaded.load(); assert(loaded.recording_shortcuts && loaded.spoiler_reset == 3);
        spit("ux0:data/vitagram/settings.json", R"({"spoiler_reset":9})");
        loaded.load(); assert(loaded.spoiler_reset == 3);
        spit("ux0:data/vitagram/settings.json", R"({"spoiler_hide_chat":false,"spoiler_hide_app":true})");
        loaded.load(); assert(loaded.spoiler_reset == 2); // Migrate the earlier two-switch layout.
    }

    // Every scroll preset persists; missing or malformed preferences retain Normal.
    {
        for (int mode = 0; mode < ScrollRepeat::kSpeedChoices; ++mode) {
            AppSettings settings; settings.scroll_speed = mode; assert(settings.save());
            AppSettings loaded; loaded.load();
            assert(loaded.loaded && loaded.scroll_speed == mode);
        }
        for (const char* json : {"{}", R"({"scroll_speed":-1})", R"({"scroll_speed":3})",
                                R"({"scroll_speed":"fast"})", R"({"scroll_speed":true})"}) {
            spit("ux0:data/vitagram/settings.json", json);
            AppSettings loaded; loaded.load(); assert(loaded.scroll_speed == 0);
        }
    }

    // Package installation requires a persisted boolean opt-in.
    {
        for (bool allow : {false, true}) {
            AppSettings settings; settings.allow_vpk_install = allow; assert(settings.save());
            AppSettings loaded; loaded.load(); assert(loaded.allow_vpk_install == allow);
        }
        for (const char* json : {"{}", R"({"allow_vpk_install":1})", R"({"allow_vpk_install":"true"})"}) {
            spit("ux0:data/vitagram/settings.json", json);
            AppSettings loaded; loaded.load(); assert(!loaded.allow_vpk_install);
        }
    }

    // Keyboard choice persists; old or malformed files retain the system keyboard.
    {
        for (bool custom : {false, true}) {
            AppSettings settings; settings.custom_keyboard = custom; assert(settings.save());
            AppSettings loaded; loaded.load(); assert(loaded.custom_keyboard == custom);
        }
        for (const char* json : {"{}", R"({"custom_keyboard":1})", R"({"custom_keyboard":"true"})"}) {
            spit("ux0:data/vitagram/settings.json", json);
            AppSettings loaded; loaded.load(); assert(!loaded.custom_keyboard);
        }
    }

    // Input-language preferences survive restart; old/malformed settings stay automatic.
    {
        AppSettings settings; settings.keyboard_languages = {"en", "ru"}; assert(settings.save());
        AppSettings loaded; loaded.load();
        assert(loaded.keyboard_languages == std::vector<std::string>({"en", "ru"}));
        for (const char* json : {"{}", R"({"keyboard_languages":true})", R"({"keyboard_languages":[1,"unknown"]})"}) {
            spit("ux0:data/vitagram/settings.json", json); loaded.load(); assert(loaded.keyboard_languages.empty());
        }
        spit("ux0:data/vitagram/settings.json", R"({"keyboard_languages":["ru","en-GB","ru",null,"unknown"]})");
        loaded.load(); assert(loaded.keyboard_languages == std::vector<std::string>({"ru", "en"}));
        loaded.keyboard_languages.clear(); assert(loaded.save()); loaded.load(); assert(loaded.keyboard_languages.empty());
    }

    struct Item { std::string path, data; } items[] = {
        {files + "/photos/a.jpg", blob(30000, 1)}, {files + "/videos/v.mp4", blob(200000, 2)},
        {db + "/stickers/s.webp", blob(9000, 3)}, {db + "/thumbnails/b1/t.jpg", blob(1, 4)},
        {db + "/ui-cache.json", "{\"chats\":{\"1\":{\"title\":\"secret chat\"}}}"},
        {"ux0:data/vitagram/portrait-432x768-0123456789abcdef.mp4", blob(70000, 5)},
    };
    for (auto& i : items) spit(i.path, i.data);
    // Not user data: TDLib's own database files and trash that is being emptied anyway.
    spit(db + "/db.sqlite", blob(5000, 6)); spit(db + "/td.binlog", blob(5000, 7)); spit(db + "/.encrypted-v2", "1");
    spit(db + "/old.trash/x.jpg", blob(100, 8));
    spit(files + "/photos/stale.jpg.vgs", "junk");
    spit("ux0:data/vitagram/settings.json", "{}");

    using namespace vault_sweep;
    assert(is_user_file(files + "/photos/a.jpg") && is_user_file("ux0:/data/vitagram/files/photos/a.jpg"));
    assert(is_user_file(db + "/stickers/s.webp") && is_user_file(db + "/ui-cache.json"));
    assert(is_db_cache(db + "/ui-cache.json") && is_db_cache(db + "/ui-history/42.json"));
    assert(!is_db_cache(db + "/stickers/s.json") && !is_db_cache(db + "/ui-history/x/42.json"));
    assert(!is_user_file(db + "/db.sqlite") && !is_user_file(db + "/td.binlog") && !is_user_file(files + "/../x"));
    assert(!is_user_file(files + "/a.vgs") && !is_user_file("ux0:data/vitagram/settings.json") && !is_user_file("/etc/passwd"));
    // A link inside the tree is not followed: files outside VitaGram's directories are never listed.
    assert(!mkdir("outside", 0777)); spit("outside/foreign.jpg", blob(500, 12));
    assert(!symlink((dir + "/outside").c_str(), (files + "/link").c_str()));
    size_t unlisted = 99;
    assert(list_user_files(&unlisted).size() == 6 && unlisted == 0 && access((files + "/photos/stale.jpg.vgs").c_str(), F_OK) != 0);

    vault::set_config_path("ux0:data/vitagram/vault.json");
    vault::load();
    assert(vault::enable(vault::Pin{0, 4, 2, 6, 7, 3, 1, 5, 4, 0, 6, 2}) == vault::Result::Ok);

    // Whole-tree migration in.
    auto outcome = convert_all(true, nullptr, nullptr, nullptr, nullptr);
    assert(outcome.completed && outcome.total == 6 && outcome.converted == 6 && outcome.failed == 0);
    for (auto& i : items) {
        // Absence of the plaintext only means something for items of 12+ bytes: random ciphertext
        // contains any given single byte about one time in six.
        assert(vault::is_sealed(i.path) && (i.data.size() < 12 || slurp(i.path).find(i.data.substr(0, 12)) == std::string::npos));
        std::string back; assert(vault::read_file(i.path, 1 << 20, back) && back == i.data);
    }
    assert(!vault::is_sealed(db + "/db.sqlite") && !vault::is_sealed(db + "/td.binlog") && !vault::is_sealed(db + "/old.trash/x.jpg"));
    assert(convert_all(true, nullptr, nullptr, nullptr, nullptr).converted == 0);          // idempotent

    // An interrupted run leaves every file either plain or fully sealed, and resumes cleanly.
    spit(files + "/photos/new1.jpg", blob(40000, 9)); spit(files + "/photos/new2.jpg", blob(40000, 10));
    int calls = 0;
    auto stop_soon = [](void* c) { return ++*static_cast<int*>(c) < 3; };
    auto partial = convert_all(true, stop_soon, &calls, nullptr, nullptr);
    assert(!partial.completed);
    for (const char* n : {"new1.jpg", "new2.jpg"}) {
        std::string p = files + "/photos/" + n; std::string back;
        assert(vault::read_file(p, 1 << 20, back) && back == blob(40000, n[3] == '1' ? 9 : 10));
        assert(access((p + ".vgs").c_str(), F_OK) != 0);
    }
    assert(convert_all(true, nullptr, nullptr, nullptr, nullptr).completed);
    assert(vault::is_sealed(files + "/photos/new1.jpg") && vault::is_sealed(files + "/photos/new2.jpg"));

    // The background sealer picks up a freshly downloaded file.
    Sealer sealer;
    sealer.start();
    spit(files + "/photos/live.jpg", blob(12345, 11));
    sealer.enqueue(files + "/photos/live.jpg");
    sealer.enqueue(db + "/db.sqlite");                                                  // ignored: not user data
    for (int i = 0; i < 50 && !vault::is_sealed(files + "/photos/live.jpg"); ++i) usleep(100000);
    assert(vault::is_sealed(files + "/photos/live.jpg") && !vault::is_sealed(db + "/db.sqlite"));
    sealer.stop();

    // Audio-only progressive readers also postpone sealing, then release it.
    const std::string streamed = files + "/photos/streamed.bin";
    spit(streamed, blob(12345, 12));
    {
        download_stream::Pin reading(true);
        sealer.start(); sealer.enqueue(streamed);
        usleep(600000);
        assert(!vault::is_sealed(streamed));
    }
    for (int i = 0; i < 50 && !vault::is_sealed(streamed); ++i) usleep(100000);
    assert(vault::is_sealed(streamed));

    // A foreground pass holds the background sealer off the files (they share "<file>.vgs").
    const std::string held = files + "/photos/held.bin";
    spit(held, blob(300000, 13));
    {
        SealerPause pause;
        assert(sealer_paused());
        sealer.enqueue(held);
        usleep(900000);
        assert(!vault::is_sealed(held) && access((held + ".vgs").c_str(), F_OK) != 0);
    }
    assert(!sealer_paused());
    for (int i = 0; i < 50 && !vault::is_sealed(held); ++i) usleep(100000);
    assert(vault::is_sealed(held));
    {
        // A conversion already running gives way before the pause returns.
        const std::string big = files + "/photos/big.bin";
        spit(big, blob(4 << 20, 14));
        sealer.enqueue(big);
        for (int i = 0; i < 100 && access((big + ".vgs").c_str(), F_OK) != 0 && !vault::is_sealed(big); ++i) usleep(5000);
        SealerPause pause;
        assert(access((big + ".vgs").c_str(), F_OK) != 0);
    }
    sealer.stop();

    // Protected state is not downgraded (F02): with the vault on and no migration pending, plain or
    // console-key settings and caches are refused instead of trusted.
    {
        vault::set_migration_pending(false);
        assert(vault::settings_must_be_sealed() && vault::caches_must_be_sealed());
        spit("ux0:data/vitagram/settings.json", R"({"proxy_enabled":true,"proxy_server":"attacker.invalid","proxy_port":8080,"proxy_type":2,"check_updates":false})");
        AppSettings forged; forged.load();
        assert(forged.rejected && !forged.proxy_enabled && forged.proxy_server.empty() && forged.check_updates);
        forged.proxy_server = "mine.example"; assert(forged.save());
        AppSettings mine; mine.load();
        assert(!mine.rejected && mine.proxy_server == "mine.example" && vault::is_vault_sealed("ux0:data/vitagram/settings.json"));
        const std::string plain_cache = db + "/forged.json", console_cache = db + "/console.json";
        spit(plain_cache, "{}");
        { vault::Writer w; assert(w.open(console_cache, true, true) && w.write("{}", 2) && w.close()); }
        std::string text;
        for (const auto& p : {plain_cache, console_cache}) {
            assert(!vault::read_file(p, 1 << 20, text, nullptr, nullptr, vault::Require::Vault));
            assert(vault::read_file(p, 1 << 20, text) && text == "{}"); // the permissive reader still works for media
            std::remove(p.c_str());
        }
        // While switching off, plain settings are expected again.
    }

    // F14: once settings were saved under the vault, a missing, refused or damaged settings.json leaves
    // the proxy policy unknown, so the network has to wait for the user.
    {
        const std::string settings_path = "ux0:data/vitagram/settings.json";
        assert(vault::settings_expected());
        AppSettings good; good.load(); assert(good.loaded && !proxy_policy_unknown(good));
        const std::string sealed = slurp(settings_path);
        std::remove(settings_path.c_str());
        AppSettings gone; gone.load(); assert(!gone.loaded && !gone.rejected && proxy_policy_unknown(gone));
        spit(settings_path, "{}");
        AppSettings plain; plain.load(); assert(plain.rejected && proxy_policy_unknown(plain));
        std::string damaged = sealed; damaged[damaged.size() / 2] ^= 1; spit(settings_path, damaged);
        AppSettings broken; broken.load(); assert(!broken.loaded && proxy_policy_unknown(broken));
        spit(settings_path, sealed);
        AppSettings back; back.load(); assert(back.loaded && back.proxy_server == "mine.example" && !proxy_policy_unknown(back));
    }

    // F13: with the signature stripped from vault.json (indistinguishable from an old version), a cache
    // that is not vault-sealed is dropped by the migration instead of being sealed and so made "trusted".
    {
        const std::string cache = db + "/ui-cache.json";
        vault::lock();
        json_t* config = json_load_file("ux0:data/vitagram/vault.json", 0, nullptr);
        assert(config && json_object_get(config, "state")); json_object_del(config, "state");
        assert(json_dump_file(config, "ux0:data/vitagram/vault.json", JSON_COMPACT) == 0); json_decref(config);
        vault::load();
        assert(vault::unlock(vault::Pin{0, 4, 2, 6, 7, 3, 1, 5, 4, 0, 6, 2}) == vault::Result::Ok);
        assert(vault::state_unauthenticated() && !vault::settings_expected() && vault::migration_pending());
        spit(cache, R"({"chats":{"1":{"title":"forged"}}})");
        auto inward = convert_all(true, nullptr, nullptr, nullptr, nullptr);
        assert(inward.clean() && access(cache.c_str(), F_OK) != 0);
        AppSettings kept; kept.load(); assert(kept.loaded && !proxy_policy_unknown(kept)); // sealed settings still count
        // Put the fixture back (sealed) for the switch-off checks below.
        vault::Writer w; const std::string data = items[4].data;
        assert(w.open(cache, true) && w.write(data.data(), data.size()) && w.close());
        vault::set_migration_pending(false);
    }

    // Switching off returns everything to plain text.
    vault::lock(); vault::load();
    assert(vault::begin_disable(vault::Pin{0, 4, 2, 6, 7, 3, 1, 5, 4, 0, 6, 2}) == vault::Result::Ok);
    // A file that cannot be converted (here its temporary output path is blocked; on the console a full
    // card) keeps the migration open: not clean, still sealed, still readable with the kept key (F03).
    const std::string stuck = files + "/photos/a.jpg";
    assert(!mkdir((stuck + ".vgs").c_str(), 0777));
    auto blocked = convert_all(false, nullptr, nullptr, nullptr, nullptr);
    assert(blocked.completed && blocked.failed == 1 && !blocked.clean());
    { std::string back; assert(vault::is_vault_sealed(stuck) && vault::read_file(stuck, 1 << 20, back) && back == blob(30000, 1)); }
    assert(!rmdir((stuck + ".vgs").c_str()));
    auto back_out = convert_all(false, nullptr, nullptr, nullptr, nullptr);
    assert(back_out.clean());
    assert(slurp("outside/foreign.jpg") == blob(500, 12)); // never touched through the link
    for (auto& i : items) assert(slurp(i.path) == i.data);
    assert(!vault::finish_disable());                  // migration still pending: the key is kept
    vault::set_migration_pending(false);
    assert(vault::finish_disable() && !vault::enabled());

    // F08-R: a link put in place of a root directory is not followed.
    assert(!rename(files.c_str(), (files + ".real").c_str()));
    assert(!symlink((dir + "/outside").c_str(), files.c_str()));
    size_t root_errors = 0;
    auto listed = list_user_files(&root_errors);
    assert(root_errors >= 1);
    for (const auto& p : listed) assert(p.find("foreign") == std::string::npos);
    assert(!unlink(files.c_str()) && !rename((files + ".real").c_str(), files.c_str()));

    assert(!chdir("/"));
    std::string cmd = "rm -rf " + dir; (void)!system(cmd.c_str());
    std::cout << "Vault sweep: tree migration both ways, TDLib files untouched, interruption safety, background sealer PASS\n";
}
