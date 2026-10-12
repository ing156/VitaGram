#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace portrait_video_cache {
// Reader exposes plaintext even when storage is encrypted. Paths and encryption
// nonces must never affect the identity of an already converted video.
template<class Reader>
bool key(Reader& reader, uint64_t& result) {
    const int64_t size=reader.size();
    if (size<=0) return false;
    uint64_t hash=1469598103934665603ULL;
    auto add=[&](uint8_t byte) { hash^=byte; hash*=1099511628211ULL; };
    for (unsigned i=0;i<8;++i) add((uint64_t)size>>(i*8));
    uint8_t sample[4096];
    for (int64_t offset : {int64_t(0),std::max<int64_t>(0,size-sizeof(sample))}) {
        if (reader.seek(offset,SEEK_SET)<0) return false;
        const size_t wanted=std::min<int64_t>(sizeof(sample),size-offset);
        size_t count=0;
        while (count<wanted) {
            const int64_t read=reader.read(sample+count,wanted-count);
            if (read<=0 || (uint64_t)read>wanted-count) return false;
            count+=(size_t)read;
        }
        for (size_t i=0;i<count;++i) add(sample[i]);
    }
    result=hash;
    return true;
}
}
