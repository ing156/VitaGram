#include "folder_drawer.h"
#include <cassert>
#include <iostream>
using chat_view::FolderDrawer;
int main() {
    FolderDrawer d;
    assert(!d.open && d.offset(0)==12 && d.shift(0)==0);
    d.show(true,100);
    float prev=12;
    for (int t=100;t<=180100;t+=1000) { float p=d.offset(t); assert(p>=prev && p<=136); prev=p; }
    assert(d.offset(180100)==136 && d.shift(180100)==124);
    const float mid=d.offset(90100);
    d.show(false,90100); assert(d.offset(90100)==mid); assert(d.offset(270100)==12 && d.shift(270100)==0);
    int chosen;
    d.reset();
    assert(!d.touch(true,200,120,20,0,chosen));
    assert(!d.touch(true,205,200,20,1000,chosen));
    assert(!d.touch(false,0,0,20,2000,chosen)); // Ordinary vertical chat scrolling stays unowned.
    assert(d.touch(true,8,200,20,3000,chosen));
    assert(d.touch(true,65,204,20,4000,chosen)); assert(d.open);
    assert(d.touch(false,0,0,20,5000,chosen)); assert(chosen==-1);
    for (int row=0;row<8;++row) {
        assert(d.hit(6,64+row*52,20)==row);
        assert(d.hit(117,64+row*52+47,20)==row);
        assert(d.hit(5,64+row*52,20)==-1);
        assert(d.hit(118,64+row*52,20)==-1);
        assert(d.hit(10,64+row*52+48,20)==-1);
    }
    assert(d.hit(10,63,20)==-1); assert(d.hit(10,480,20)==-1);
    assert(d.touch(true,20,64+2*52,20,6000,chosen));
    assert(d.touch(false,0,0,20,7000,chosen)); assert(chosen==2 && !d.open);
    d.show(true,8000);
    assert(d.touch(true,500,200,20,9000,chosen));
    assert(d.touch(false,0,0,20,10000,chosen)); assert(!d.open && chosen==-1);
    d.show(true,11000);
    assert(d.touch(true,50,200,20,12000,chosen));
    assert(d.touch(true,50,100,20,13000,chosen)); assert(d.first==1);
    d.reveal(20);assert(d.first==1 && d.focus>=1 && d.focus<9);
    assert(d.touch(false,0,0,20,14000,chosen)); assert(chosen==-1 && d.open);
    assert(d.touch(true,100,200,20,15000,chosen));
    assert(d.touch(true,40,204,20,16000,chosen)); assert(!d.open);
    assert(d.touch(false,0,0,20,17000,chosen)); assert(chosen==-1);
    d.focus=19;d.reveal(20);assert(d.first==12);
    d.reveal(1);assert(d.focus==0 && d.first==0);
    assert(d.hit(10,116,1)==-1); assert(d.hit(10,64,0)==-1);
    d.show(true,20000); d.show(false,200000);
    assert(d.touch(true,80,200,20,201000,chosen)); // Visible panel still owns taps while closing.
    assert(d.touch(false,0,0,20,202000,chosen)); assert(d.open && chosen==-1);
    for (bool expanded : {false, true}) {
        d.reset(); d.show(expanded,0);
        for (int y : {0, 25, FolderDrawer::top-1}) {
            assert(FolderDrawer::contentShift(y,136)==0);
            assert(!d.touch(true,8,y,20,180000,chosen));
            assert(!d.touch(false,0,0,20,180001,chosen));
            assert(d.open==expanded);
        }
        assert(FolderDrawer::contentShift(FolderDrawer::top,136)==136);
        assert(FolderDrawer::contentShift(543,12)==12);
        assert(d.touch(true,10,FolderDrawer::top,20,200000,chosen));
        assert(d.touch(false,0,0,20,200001,chosen));
        assert(d.open!=expanded); // Return control lives below the fixed header.
    }
    d.reset();assert(!d.open && !d.down && !d.owned && d.offset(0)==12);
    std::cout << "Folder drawer animation, hit boundaries, scrolling and gesture ownership: PASS\n";
}
