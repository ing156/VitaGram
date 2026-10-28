#include "vita_net.h"
#include "i18n.h"
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <cassert>
#include <cstring>
#include <cstdio>

int polls = 0, flight = 0, wifi = 1, state = SCE_NETCTL_STATE_CONNECTED;
int sceSysmoduleLoadModule(int) { return 0; }
int sceSysmoduleUnloadModule(int) { return 0; }
int sceNetInit(SceNetInitParam*) { return 0; }
int sceNetTerm() { return 0; }
int sceNetCtlInit() { return 0; }
int sceNetCtlTerm() { return 0; }
int sceNetCtlInetGetState(int* result) { ++polls; *result = state; return 0; }
int sceRegMgrGetKeyInt(const char*, const char* key, int* result) {
    ++polls; *result = !std::strcmp(key, "flight_mode") ? flight : wifi; return 0;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    i18n::init(argv[1], "en");
    VitaNet net;
    assert(net.init());
    struct Case { int flight, wifi, state; const char* key; };
    for (const Case& value : {Case{0,1,3,"net.online"}, Case{0,1,1,"net.connecting"},
            Case{0,1,0,"net.disconnected"}, Case{0,1,-1,"net.unknown"},
            Case{1,1,3,"net.airplane"}, Case{0,0,3,"net.online"},
            Case{0,0,1,"net.connecting"}, Case{0,0,2,"net.connecting"},
            Case{0,0,0,"net.disconnected"}, Case{0,0,-1,"net.unknown"}}) {
        flight = value.flight; wifi = value.wifi; state = value.state;
        net.poll();
        const int sampled = polls;
        for (const auto& language : i18n::languages()) {
            assert(i18n::select(language.code));
            net.refreshLanguage();
            assert(net.status() == tr(value.key));
            assert(polls == sampled); // Online must not wait for the one-second device poll.
        }
    }
    std::puts("Network language: all connection states and locales refresh immediately without polling");
}
