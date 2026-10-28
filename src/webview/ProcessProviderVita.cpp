#include "ProcessProviderVita.h"
#include "webapp_diagnostics.h"
#include "VitaThreading.h"
#include <wpe/wpe.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/socket.h>
#include <unordered_map>
#include <unistd.h>

namespace WTF {
void setCurrentThreadIsAuxiliaryProcessMain(bool);
}

namespace WebKit {
int WebProcessMain(int, char**);
int NetworkProcessMain(int, char**);
}

namespace {
struct Launch {
    wpe_process_type type;
    int socket;
    char identifier[24];
};

struct Provider {
    struct Process { SceUID thread; int socket; };
    std::mutex lock;
    std::unordered_map<int64_t, Process> threads;
    unsigned launched = 0;
};

int process_thread(SceSize args, void* argument)
{
    if (args != sizeof(Launch*) || !argument)
        return -1;
    auto* launch = *static_cast<Launch**>(argument);
    char socket[24] { };
    std::snprintf(socket, sizeof(socket), "%d", launch->socket);
    char program[] = "vita-webkit-component";
    char* argv[] = { program, launch->identifier, socket, nullptr };
    int result = -1;
    webapp_diagnostics::record(webapp_diagnostics::Stage::ProcessStarted, launch->type);
    WTF::setCurrentThreadIsAuxiliaryProcessMain(true);
    if (launch->type == WPE_PROCESS_TYPE_WEB)
        result = WebKit::WebProcessMain(3, argv);
    else if (launch->type == WPE_PROCESS_TYPE_NETWORK)
        result = WebKit::NetworkProcessMain(3, argv);
    WTF::setCurrentThreadIsAuxiliaryProcessMain(false);
    delete launch;
    return result;
}

void* create_provider(wpe_process_provider*) { return new Provider; }

void terminate_process(void* data, int64_t process)
{
    auto& provider = *static_cast<Provider*>(data);
    Provider::Process child { -1, -1 };
    {
        std::lock_guard guard(provider.lock);
        auto found = provider.threads.find(process);
        if (found == provider.threads.end())
            return;
        child = found->second;
        provider.threads.erase(found);
    }
    // Closing the explicit IPC endpoint asks AuxiliaryProcessMain's run loop
    // to shut down. Vita user threads have no safe asynchronous kill API.
    shutdown(child.socket, SHUT_RDWR);
    close(child.socket);
    // Let the UI keep servicing shutdown messages; never delete a running thread.
    // shortcut: retain timed-out handles until app exit; reap them if process restarts are supported.
    SceUInt timeout = 1000;
    const int wait_result = sceKernelWaitThreadEnd(child.thread, nullptr, &timeout);
    if (wait_result >= 0)
        sceKernelDeleteThread(child.thread);
}

void destroy_provider(void* data)
{
    auto* provider = static_cast<Provider*>(data);
    for (;;) {
        int64_t process = -1;
        {
            std::lock_guard guard(provider->lock);
            if (provider->threads.empty())
                break;
            process = provider->threads.begin()->first;
        }
        terminate_process(provider, process);
    }
    delete provider;
}

int64_t launch_process(void* data, wpe_process_type type, void* opaque)
{
    webapp_diagnostics::record(webapp_diagnostics::Stage::ProcessLaunch, type);
    auto& provider = *static_cast<Provider*>(data);
    auto* options = static_cast<wpe_vita_process_launch_options*>(opaque);
    if (!options || options->ipc_socket < 0 || !options->process_identifier
        || (type != WPE_PROCESS_TYPE_WEB && type != WPE_PROCESS_TYPE_NETWORK))
        return -1;
    std::lock_guard guard(provider.lock);
    const unsigned bit = type == WPE_PROCESS_TYPE_WEB ? 1u : 2u;
    // WebKit's component singletons cannot be initialized a second time in the same SELF.
    if (provider.launched & bit) {
        webapp_diagnostics::record(webapp_diagnostics::Stage::Failed, -EALREADY);
        return -1;
    }
    // The manager keeps the original descriptor so it can shutdown the socket
    // without racing the Connection object's eventual close in the child.
    int process_socket = dup(options->ipc_socket);
    if (process_socket < 0)
        return -1;
    auto* launch = new Launch { type, process_socket, { } };
    std::snprintf(launch->identifier, sizeof(launch->identifier), "%s", options->process_identifier);
    const char* name = type == WPE_PROCESS_TYPE_WEB ? "VitaGram Web" : "VitaGram Network";
    SceUID thread = sceKernelCreateThread(name, process_thread, 0x60, 1024 * 1024, VITAGRAM_WK_THREAD_VM, 0, nullptr);
    if (thread < 0) {
        webapp_diagnostics::record(webapp_diagnostics::Stage::Failed, thread);
        close(process_socket);
        delete launch;
        return -1;
    }
    Launch* argument = launch;
    int result = sceKernelStartThread(thread, sizeof(argument), &argument);
    if (result < 0) {
        webapp_diagnostics::record(webapp_diagnostics::Stage::Failed, result);
        sceKernelDeleteThread(thread);
        close(process_socket);
        delete launch;
        return -1;
    }
    provider.launched |= bit;
    provider.threads.emplace(thread, Provider::Process { thread, options->ipc_socket });
    return thread;
}

const wpe_process_provider_interface provider_interface = {
    .create = create_provider,
    .destroy = destroy_provider,
    .launch = launch_process,
    .terminate = terminate_process,
    ._wpe_reserved1 = nullptr,
    ._wpe_reserved2 = nullptr,
    ._wpe_reserved3 = nullptr,
    ._wpe_reserved4 = nullptr,
    ._wpe_reserved5 = nullptr,
};
}

bool register_vita_process_provider()
{
    static const bool registered = [] {
        wpe_process_provider_register_interface(&provider_interface);
        return true;
    }();
    return registered;
}
