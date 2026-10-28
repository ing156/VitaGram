#include "chat_view_layout.h"
#include <cassert>
#include <iostream>
#include <utility>

int main() {
    for (bool combined : {false, true}) for (bool folders : {false, true}) {
        const auto view = chat_view::layout(combined, folders);
        for (int count = 0; count <= 7; ++count) {
            assert(view.attachLeft() == (combined ? 264 : 40));
            assert(view.attachLeft()-4 >= view.left());
            assert(view.attachLeft()+ui_layout::attach_width+4 <= 960);
            assert(view.attachTop(count)-ui_layout::menu_pad-4 >= view.top);
            for (int y = view.attachTop(count)-10; y <= ui_layout::menu_bottom+10; ++y)
                for (int x : {0, 40, 223, 224, view.attachLeft()-1, view.attachLeft(),
                        view.attachLeft()+299, view.attachLeft()+300, 959}) {
                    const int row = y-view.attachTop(count);
                    const bool hit = x >= view.attachLeft() && x < view.attachLeft()+300 &&
                        row >= 0 && row < count*40 && row%40 >= 2 && row%40 < 38;
                    assert(view.attachHit(x,y,count) == (hit ? row/40 : -1));
                }
        }
        assert(view.bubbleWidth() == (combined ? 672 : 712));
        assert(view.bodyWidth() + 32 == view.bubbleWidth());
        assert(view.bubbleLeft() + view.bubbleWidth() <= ui_layout::bubble_right);
        assert(view.top == 71);
        assert(view.listLeft()+view.listWidth() == (combined ? 214 : 950));
        assert(view.listInset() == (combined && folders ? 12 : 0));
        // The rail changes only the chat rows, never the message pane or its right edge.
        assert(view.left() == (combined ? 224 : 0));
        assert(view.x(30)+view.width(900) == 930);
        assert(view.listTop() == (folders && !combined ? 151 : 103));
        assert(view.folderTop() == (combined ? 71 : 103));
        assert(view.searchTop() == 71);
        assert(view.searchTop() + ui_layout::search_height <= view.listTop());
        for (const auto& span : {std::pair<int,int>{24,912}, {30,900}, {160,640}, {38,884}, {32,896}}) {
            assert(view.x(span.first) >= view.left());
            assert(view.x(span.first)+view.width(span.second) <= 960);
            assert(view.x(span.first)+view.width(span.second)/2 == view.center());
        }
        assert(view.y(71) == view.top);
        assert(view.y(92) >= view.top);
        assert(view.x(184)+view.width(592) == 776);
        assert(view.topicTop()+view.topicRows()*50+54 <= 544);
        if (combined) assert(ui_layout::menu_bottom-view.menuRows()*ui_layout::menu_row-2*ui_layout::menu_pad-4 >= view.top);
        else assert(view.menuRows() == ui_layout::menu_max_rows);
        for (int row = 0; row < view.topicRows(); ++row) {
            const int y = view.topicTop()+row*50;
            assert(view.topicHit(view.x(20), y) == row);
            assert(view.topicHit(939, y+47) == row);
            assert(view.topicHit(view.x(20)-1, y) == -1);
            assert(view.topicHit(940, y) == -1);
            assert(view.topicHit(view.x(20), y+48) == -1);
            assert(view.topicHit(view.x(20), y+49) == -1);
        }
        assert(view.topicHit(view.x(20), view.topicTop()+view.topicRows()*50) == -1);
        for (int row = 0; row < 5; ++row) {
            const int y = view.y(ui_layout::poll_top)+row*ui_layout::poll_step;
            assert(view.pollHit(view.x(ui_layout::poll_left), y) == row);
            assert(view.pollHit(view.x(ui_layout::poll_left)-1, y) == -1);
            assert(view.pollHit(934, y) == -1);
            assert(view.pollHit(view.x(ui_layout::poll_left), y+ui_layout::poll_height) == -1);
        }
        // Conservative glyph bounds at the largest supported font (1.3).
        assert(view.y(ui_layout::poll_question_baseline)-24 >= view.top+22);
        assert(ui_layout::poll_question_pitch >= 24+2);
        assert(ui_layout::poll_question_baseline+ui_layout::poll_question_pitch+6 <= ui_layout::poll_kind_baseline-20);
        assert(ui_layout::poll_kind_baseline+6 <= ui_layout::poll_top);
        assert(ui_layout::poll_top+4*ui_layout::poll_step+ui_layout::poll_height <= 467-20);
        assert(22+6 <= 44-16);
        assert(44+4 <= ui_layout::poll_height);
        assert(view.pollHit(view.x(ui_layout::poll_left), view.y(ui_layout::poll_top)+5*ui_layout::poll_step) == -1);
        assert(view.searchHit(view.listLeft(), view.searchTop()));
        assert(!view.searchHit(view.listLeft()-1, view.searchTop()));
        assert(!view.searchHit(view.listLeft()+view.listWidth(), view.searchTop()));
        assert(!view.searchHit(view.listLeft(), view.searchTop()-1));
        assert(!view.searchHit(view.listLeft(), view.searchTop()+ui_layout::search_height));
        if (combined && folders) {
            assert(view.searchTop() == 71);
            assert(!view.folderBandHit(10, view.searchTop()));
            assert(!view.folderBandHit(10, view.folderTop()));
        }
        for (int bottom : {312, 494, 544}) {
            assert(view.historyHit(view.left(), view.top, bottom));
            assert(!view.historyHit(view.left()-1, view.top, bottom));
            assert(!view.historyHit(960, view.top, bottom));
            assert(!view.historyHit(view.center(), view.top-1, bottom));
            assert(!view.historyHit(view.center(), bottom, bottom));
            // Pointer routing has no dependency on the controller's focused pane.
            for (int y = 0; y <= 544; ++y) for (int x : {-1, 0, 9, 10, 213, 214, 223, 224, 225, 959, 960}) {
                const auto pane = view.scrollPane(x, y, bottom);
                if (!combined) assert(pane == chat_view::Pane::None);
                else if (x >= view.listInset() && x < chat_view::sidebar && y >= view.listTop() && y < 544)
                    assert(pane == chat_view::Pane::Chats);
                else if (x >= chat_view::sidebar && x < 960 && y >= view.top && y < bottom)
                    assert(pane == chat_view::Pane::History);
                else assert(pane == chat_view::Pane::None);
            }
            if (combined) {
                assert(view.scrollPane(20, view.searchTop(), bottom) == chat_view::Pane::None);
                assert(view.scrollPane(300, view.searchTop(), bottom) == chat_view::Pane::History);
                assert(view.scrollPane(20, view.listTop()-1, bottom) == chat_view::Pane::None);
            }
        }
        for (int offset : {-100, -7, 0, 7, 100}) for (int y = 151; y < 544; ++y) {
            const bool painted = y-151+offset >= 0 && (y-151+offset)%43 < 41;
            assert(view.listHit(view.listLeft(), y, 151, 43, offset) == painted);
            assert(!view.listHit(view.listLeft()-1, y, 151, 43, offset));
            assert(!view.listHit(view.listLeft()+view.listWidth(), y, 151, 43, offset));
        }
        assert(!view.listHit(view.listLeft(), 150, 151, 43, 0));
        assert(!view.listHit(view.listLeft(), 544, 151, 43, 0));
    }
    std::cout << "Classic/combined geometry and touch boundaries: PASS\n";
}
