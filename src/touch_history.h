#pragma once
#include <psp2/touch.h>
#include <algorithm>
#include <array>
#include <cstdint>

class TouchHistory {
public:
    void ignore(const SceTouchData* latest) {
        last_ = latest ? latest->timeStamp : 0;
        if (!latest) samples = {};
    }
    template<class Read> unsigned poll(const SceTouchData* latest, Read read) {
        if (!latest || !latest->timeStamp) return 0;
        if (!last_ || latest->timeStamp <= last_) {
            last_ = latest->timeStamp;
            return 0;
        }
        // Read only after Peek confirms a newer sample, avoiding a wait for the next scan.
        const int count = read(samples.data(), capacity);
        const unsigned end = count > 0 ? std::min<unsigned>(count,capacity) : 0;
        std::sort(samples.begin(),samples.begin()+end,[](const auto& a,const auto& b) { return a.timeStamp<b.timeStamp; });
        unsigned kept=0;
        for (unsigned i=0;i<end;++i) {
            const auto sample=samples[i];
            if (sample.timeStamp<=last_) continue;
            last_=sample.timeStamp;
            if (sample.reportNum<=SCE_TOUCH_MAX_REPORT) samples[kept++]=sample;
        }
        if (latest->timeStamp>last_ && latest->reportNum<=SCE_TOUCH_MAX_REPORT) {
            samples[kept++]=*latest;
        }
        last_=std::max<uint64_t>(last_,latest->timeStamp);
        return kept;
    }
    static constexpr unsigned capacity=64;
    std::array<SceTouchData,capacity+1> samples{}; // Spare slot for the latest Peek sample.
private:
    uint64_t last_=0;
};
