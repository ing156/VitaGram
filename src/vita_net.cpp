#include "vita_net.h"
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/registrymgr.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>

VitaNet::VitaNet() : memory_(nullptr), net_ready_(false), ctl_ready_(false) {
    error_[0] = 0;
}

VitaNet::~VitaNet() { shutdown(); }

bool VitaNet::init() {
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

    const int size = 1024 * 1024;
    memory_ = std::malloc(size);
    if (!memory_) {
        std::snprintf(error_, sizeof(error_), "network memory allocation failed");
        return false;
    }

    SceNetInitParam p;
    std::memset(&p, 0, sizeof(p));
    p.memory = memory_;
    p.size = size;
    p.flags = 0;

    int rc = sceNetInit(&p);
    if (rc < 0) {
        std::snprintf(error_, sizeof(error_), "sceNetInit: 0x%08X", rc);
        return false;
    }
    net_ready_ = true;

    rc = sceNetCtlInit();
    if (rc < 0) {
        std::snprintf(error_, sizeof(error_), "sceNetCtlInit: 0x%08X", rc);
        return false;
    }
    ctl_ready_ = true;
    return true;
}

void VitaNet::shutdown() {
    if (ctl_ready_) {
        sceNetCtlTerm();
        ctl_ready_ = false;
    }
    if (net_ready_) {
        sceNetTerm();
        net_ready_ = false;
    }
    if (memory_) {
        std::free(memory_);
        memory_ = nullptr;
    }
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
}

void VitaNet::poll() {
    int flight = -1, wifi = -1, state = -1;
    if (sceRegMgrGetKeyInt("/CONFIG/SYSTEM/", "flight_mode", &flight) < 0) flight = -1;
    if (sceRegMgrGetKeyInt("/CONFIG/NET/", "wifi_flag", &wifi) < 0) wifi = -1;
    if (ctl_ready_ && sceNetCtlInetGetState(&state) < 0) state = -1;
    const char* connection = state == SCE_NETCTL_STATE_CONNECTED ? "Online" :
        state == SCE_NETCTL_STATE_CONNECTING || state == SCE_NETCTL_STATE_FINALIZING ? "Connecting" :
        state == SCE_NETCTL_STATE_DISCONNECTED ? "Disconnected" : "Connection unknown";
    const char* indicator = flight > 0 ? "Airplane mode" : wifi == 0 ? "Wi-Fi off" : connection;
    std::snprintf(status_, sizeof(status_), "%s", indicator);
}
