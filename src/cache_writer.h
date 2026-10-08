#pragma once
#include "vault.h"
#include <jansson.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <psp2/kernel/threadmgr.h>

namespace cache_writer {
enum class Result { Ok, Limit, Interrupted, Error };
using Proceed = bool (*)(void*);
// Aggregate tiny Jansson callbacks into bounded writes and yield between blocks.
// Only the background cache owner uses this; no UI/cache mutex may be held.
struct Output {
    vault::Writer* file;
    size_t limit, total = 0, used = 0;
    Proceed proceed; void* context;
    Result result = Result::Ok;
    std::array<char, 64*1024> buffer{};
    bool drain() {
        if (proceed && !proceed(context)) { result = Result::Interrupted; return false; }
        if (used && !file->write(buffer.data(), used)) { result = Result::Error; return false; }
        used = 0;
        sceKernelDelayThread(1000);
        return true;
    }
    static int append(const char* data, size_t length, void* opaque) {
        auto& out = *static_cast<Output*>(opaque);
        if (length > out.limit-out.total) { out.result = Result::Limit; return -1; }
        out.total += length;
        while (length) {
            size_t n = std::min(length, out.buffer.size()-out.used);
            std::memcpy(out.buffer.data()+out.used, data, n);
            out.used += n; data += n; length -= n;
            if (out.used == out.buffer.size() && !out.drain()) return -1;
        }
        return 0;
    }
};
inline Result write(json_t* value, const char* temp, size_t limit, Proceed proceed = nullptr, void* context = nullptr) {
    if (proceed && !proceed(context)) return Result::Interrupted;
    // Sealed whenever the vault is on; the snapshot never reaches the card in plain text then.
    vault::Writer writer;
    if (!writer.open(temp)) return Result::Error;
    // Our buffer supplies the batching; avoid an extra libc buffer/copy.
    writer.set_unbuffered();
    Output out{&writer, limit, 0, 0, proceed, context, Result::Ok, {}};
    if (json_dump_callback(value, Output::append, &out, JSON_COMPACT) && out.result == Result::Ok)
        out.result = Result::Error;
    if (out.result == Result::Ok) out.drain();
    if (!writer.close() && out.result == Result::Ok) out.result = Result::Error;
    if (out.result == Result::Ok && proceed && !proceed(context)) out.result = Result::Interrupted;
    if (out.result != Result::Ok) std::remove(temp);
    return out.result;
}
}
