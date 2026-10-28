#include "ProcessIdentifier.h"
#include <wtf/RunLoop.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <set>
#include <thread>
#include <vector>

namespace WTF {
thread_local RunLoop* owner = nullptr;
RunLoop& RunLoop::mainSingleton() { assert(owner); return *owner; }
}

int main() {
    using namespace WebCore;
    WTF::RunLoop ui, web, network;
    WTF::owner = &ui;
    const auto uiID = Process::identifier();
    const auto webID = Process::generateIdentifier();
    const auto networkID = Process::generateIdentifier();
    assert(uiID != webID && uiID != networkID && webID != networkID);
    std::atomic<unsigned> initialized{0};
    auto process = [&](WTF::RunLoop* owner, ProcessIdentifier id) {
        WTF::owner = owner;
        Process::setIdentifier(id);
        initialized.fetch_add(1);
        while (initialized.load() != 2) std::this_thread::yield();
        for (unsigned i = 0; i < 1000; ++i) assert(Process::identifier() == id);
        std::thread worker([=] {
            WTF::owner = owner;
            assert(Process::identifier() == id);
        });
        worker.join();
    };
    std::thread first(process, &web, webID), second(process, &network, networkID);
    first.join(); second.join();
    assert(Process::identifier() == uiID);
    std::vector<ProcessIdentifier> generated(1000, uiID);
    std::thread a([&] { for (unsigned i = 0; i < 500; ++i) generated[i] = Process::generateIdentifier(); });
    std::thread b([&] { for (unsigned i = 500; i < 1000; ++i) generated[i] = Process::generateIdentifier(); });
    a.join(); b.join();
    std::set<uint64_t> unique;
    for (auto id : generated) assert(unique.insert(id.toUInt64()).second);
    std::puts("WebKit: UI/Web/Network identities, inherited worker ownership and concurrent ID generation passed");
}
