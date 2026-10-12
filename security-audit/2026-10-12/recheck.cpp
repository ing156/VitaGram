#include "vault.h"
#include "vault_sweep.h"
#include "settings.h"
#include "tgs_policy.h"
#include "cpu_scheduler.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cassert>
#include <iostream>
#include <unistd.h>
namespace fs=std::filesystem;
int sceKernelDelayThread(unsigned us){return usleep(us);}
namespace cpu_scheduler {Thread::Thread(Role):id_(0){} Thread::~Thread(){} bool videoActive(){return false;}}
static void put(const std::string& p,const std::string& s){std::ofstream f(p);f<<s;f.close();assert(f);}
static std::string get(const std::string& p){std::ifstream f(p);std::ostringstream o;o<<f.rdbuf();return o.str();}
int main(int argc,char** argv){
    assert(argc==2 && chdir(argv[1])==0);
    fs::create_directories("ux0:data/vitagram/files");fs::create_directories("ux0:data/vitagram/db");
    vault::set_config_path("vault.json");vault::load();
    const vault::Pin pin{0,4,2,6,7,3,1,5,4,0,6,2};assert(vault::enable(pin)==vault::Result::Ok);
    // Capture a genuine signed state before the proxy settings have ever been saved.
    const std::string initial=get("vault.json");
    vault::set_migration_pending(false);
    AppSettings good;good.proxy_enabled=true;good.proxy_server="required.invalid";assert(good.save());
    assert(vault::settings_expected());
    fs::remove("ux0:data/vitagram/settings.json");
    AppSettings missing;missing.load();assert(proxy_policy_unknown(missing));
    std::cout<<"F14 original: missing expected settings blocks network policy PASS\n";
    vault::lock();
    json_t* state=json_load_file("vault.json",0,nullptr);assert(state);json_object_del(state,"state");
    assert(json_dump_file(state,"vault.json",JSON_COMPACT)==0);json_decref(state);
    put("ux0:data/vitagram/db/ui-cache.json",R"({"audit":"forged cache"})");
    vault::load();assert(vault::unlock(pin)==vault::Result::Ok && vault::state_unauthenticated());
    auto clean=vault_sweep::convert_all(true,nullptr,nullptr,nullptr,nullptr);
    assert(clean.clean() && !fs::exists("ux0:data/vitagram/db/ui-cache.json"));
    std::cout<<"F13 original: unsigned state causes cache removal by sweep PASS\n";
    std::string mixed=R"({"w":512,"h":512,"fr":30,"ip":0,"op":60,"assets":[{"id":"raster","layers":[],"p":"data:image/png;base64,AAAA","e":1}],"layers":[]})";
    assert(!tgs_policy::acceptable(mixed));
    assert(!tgs_policy::acceptable(R"({"w":512,"w":512,"h":512,"fr":30,"ip":0,"op":60,"layers":[]})"));
    std::cout<<"F15 original: mixed raster asset and duplicate keys rejected PASS\n";
    // Replay authentic metadata rather than editing its protected fields.
    vault::lock();put("vault.json",initial);
    put("ux0:data/vitagram/db/ui-cache.json",R"({"audit":"replayed-state forged cache"})");
    vault::load();assert(vault::unlock(pin)==vault::Result::Ok && !vault::state_unauthenticated());
    AppSettings replay;replay.load();
    assert(!replay.loaded && !replay.proxy_enabled && !proxy_policy_unknown(replay));
    std::cout<<"F16: genuine old settings:none state bypasses network hold after settings removal CONFIRMED\n";
    auto migrated=vault_sweep::convert_all(true,nullptr,nullptr,nullptr,nullptr);assert(migrated.clean());
    vault::set_migration_pending(false);std::string out;
    assert(vault::read_file("ux0:data/vitagram/db/ui-cache.json",1024,out,nullptr,nullptr,vault::Require::Vault));
    assert(out==R"({"audit":"replayed-state forged cache"})");
    std::cout<<"F16: genuine old migration state re-seals forged cache CONFIRMED\n";
    fs::remove("ux0:data/vitagram/files");fs::create_directory("outside");put("outside/foreign","outside fixture");
    fs::create_directory_symlink(fs::absolute("outside"),"ux0:data/vitagram/files");
    auto linked=vault_sweep::convert_all(true,nullptr,nullptr,nullptr,nullptr);
    assert(!linked.clean() && !vault::is_sealed("outside/foreign"));
    std::cout<<"F08 root symlink: migration refuses root without touching target PASS\n";
    vault::lock();
}
