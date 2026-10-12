#include "portrait_video_cache.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

struct Reader {
    std::vector<uint8_t> bytes;
    size_t position=0, grain=4096;
    bool failed=false;
    int64_t size() const { return bytes.size(); }
    int64_t seek(int64_t offset, int) { position=offset; return failed ? -1 : offset; }
    int64_t read(void* out, size_t length) {
        if (failed) return -1;
        size_t count=std::min({length,grain,bytes.size()-position});
        std::memcpy(out,bytes.data()+position,count); position+=count;
        return count;
    }
};

int main() {
    Reader original;
    original.bytes.resize(10000);
    for (size_t i=0;i<original.bytes.size();++i) original.bytes[i]=i*37;
    uint64_t first=0, second=0;
    assert(portrait_video_cache::key(original,first));
    // Reopened/moved/re-encrypted storage exposes the same plaintext through Reader.
    Reader reopened=original;
    reopened.grain=53;
    assert(portrait_video_cache::key(reopened,second) && first==second);
    reopened.bytes.back()^=1;
    assert(portrait_video_cache::key(reopened,second) && first!=second);
    reopened=original; reopened.bytes.push_back(0);
    assert(portrait_video_cache::key(reopened,second) && first!=second);
    reopened.failed=true;
    second=123;
    assert(!portrait_video_cache::key(reopened,second) && second==123);
    Reader empty;
    assert(!portrait_video_cache::key(empty,second));
    Reader small; small.bytes={1,2,3};
    assert(portrait_video_cache::key(small,first));
    small.grain=1;
    assert(portrait_video_cache::key(small,second) && first==second);
    std::cout << "Portrait cache identity, short reads and read failures: PASS\n";
}
