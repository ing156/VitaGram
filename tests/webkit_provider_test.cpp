#include "webview/ProcessProviderVita.h"
#include <wpe/wpe.h>
#include <psp2/kernel/threadmgr.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <unistd.h>

static const wpe_process_provider_interface* providerInterface;
static SceKernelThreadEntry entry;
static bool createFails, startFails, waitFails;
static unsigned created, deleted, waited, webRuns, networkRuns;
extern "C" void wpe_process_provider_register_interface(const wpe_process_provider_interface* value) {
    providerInterface = value;
}
extern "C" SceUID sceKernelCreateThread(const char*, SceKernelThreadEntry callback, int, SceSize, SceUInt attr, int, const SceKernelThreadOptParam*) {
    assert(attr == 0x04000000u); entry = callback;
    return createFails ? -1 : static_cast<int>(++created);
}
extern "C" int sceKernelStartThread(SceUID, SceSize size, void* data) {
    if (startFails) return -1;
    assert(entry(size, data) == 0); return 0;
}
extern "C" int sceKernelWaitThreadEnd(SceUID, int*, SceUInt* timeout) {
    assert(timeout && *timeout == 1000); ++waited;
    return waitFails ? -1 : 0;
}
extern "C" int sceKernelDeleteThread(SceUID) { ++deleted; return 0; }
namespace WTF { void setCurrentThreadIsAuxiliaryProcessMain(bool) { } }
namespace WebKit {
int WebProcessMain(int argc, char** argv) { assert(argc == 3); ++webRuns; close(std::atoi(argv[2])); return 0; }
int NetworkProcessMain(int argc, char** argv) { assert(argc == 3); ++networkRuns; close(std::atoi(argv[2])); return 0; }
}

int main() {
    assert(register_vita_process_provider() && providerInterface);
    auto* provider = providerInterface->create(nullptr);
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    wpe_vita_process_launch_options launch{sockets[0], "2"};
    assert(providerInterface->launch(provider, WPE_PROCESS_TYPE_WEB, nullptr) < 0);
    assert(providerInterface->launch(provider, WPE_PROCESS_TYPE_GPU, &launch) < 0);
    createFails = true;
    assert(providerInterface->launch(provider, WPE_PROCESS_TYPE_WEB, &launch) < 0);
    createFails = false; startFails = true;
    assert(providerInterface->launch(provider, WPE_PROCESS_TYPE_WEB, &launch) < 0);
    assert(deleted == 1 && webRuns == 0);
    startFails = false;
    auto web = providerInterface->launch(provider, WPE_PROCESS_TYPE_WEB, &launch);
    assert(web > 0 && webRuns == 1);
    assert(providerInterface->launch(provider, WPE_PROCESS_TYPE_WEB, &launch) < 0);
    waitFails = true;
    providerInterface->terminate(provider, web);
    assert(waited == 1 && deleted == 1);
    providerInterface->terminate(provider, web);
    assert(waited == 1);
    assert(providerInterface->launch(provider, WPE_PROCESS_TYPE_WEB, &launch) < 0);
    close(sockets[1]);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    launch.ipc_socket = sockets[0]; launch.process_identifier = "3";
    auto network = providerInterface->launch(provider, WPE_PROCESS_TYPE_NETWORK, &launch);
    assert(network > 0 && networkRuns == 1);
    waitFails = false;
    providerInterface->destroy(provider);
    assert(waited == 2 && deleted == 2);
    close(sockets[1]);
    std::puts("WebKit provider: failure retry, singleton restart rejection and bounded wait without deleting running threads passed");
}
