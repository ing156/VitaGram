#include "text_layout_cache.h"
#include <cassert>
#include <iostream>
int main() {
    TextWidthCache cache;int calls=0;
    auto measure=[&]{++calls;return 42;};
    assert(cache.get("hello",1,0.5f,measure)==42);
    assert(cache.get("hello",1,0.5f,measure)==42 && calls==1);
    cache.get("hello",2,0.5f,measure);cache.get("hello",2,0.6f,measure);cache.get("changed",2,0.6f,measure);
    assert(calls==4);
    cache.clear();cache.get("hello",1,0.5f,measure);assert(calls==5);
    std::string large(4097,'x');cache.get(large.c_str(),1,1,measure);cache.get(large.c_str(),1,1,measure);assert(calls==7);
    for (int i=0;i<1000;++i) {
        std::string text=std::to_string(i)+std::string(3000,'a');
        cache.get(text.c_str(),1,1,measure);
        assert(cache.size()<=128 && cache.bytes()<=128*1024);
    }
    TextWidthCache lru;int lru_calls=0;
    auto lru_measure=[&]{++lru_calls;return 1;};
    for (int i=0;i<128;++i) lru.get(std::to_string(i).c_str(),1,1,lru_measure);
    lru.get("0",1,1,lru_measure);lru.get("new",1,1,lru_measure);lru.get("0",1,1,lru_measure);
    assert(lru_calls==129);
    lru.get("1",1,1,lru_measure);assert(lru_calls==130);
    int probes=0;
    auto width=[&](const std::string& text){++probes;int count=0;for(unsigned char c:text) if ((c&0xc0)!=0x80)++count;return count;};
    assert(fit_utf8_text("abc",3,width)=="abc");
    probes=0;assert(fit_utf8_text(std::string(10000,'x'),23,width)==std::string(20,'x')+"...");assert(probes<=16);
    assert(fit_utf8_text("АБВГДЕ",5,width)=="АБ...");
    assert(fit_utf8_text("😀😁😂😃😄😅",5,width)=="😀😁...");
    assert(fit_utf8_text("abc",0,width)=="...");
    std::cout << "Text layout: exact cache keys, invalidation, limits, logarithmic fitting and UTF-8 boundaries passed\n";
}
