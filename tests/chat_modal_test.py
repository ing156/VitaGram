"""Compile the production visibility predicates and exercise modal transitions."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
main = (root / 'src/main.cpp').read_text()
names = ('tool_window_open', 'chat_modal_open', 'chat_ui_visible', 'tool_menu_visible', 'combined_visible')
predicates = []
for name in names:
    start = main.index('    auto ' + name + ' = [&]() {')
    predicates.append(main[start:main.index('    };', start) + 6])
fixture = r'''
#include "chat_view_layout.h"
#include "history_scroll.h"
#include <cassert>
#include <string>
#include <iostream>
#include <vector>
struct Active { bool on = false; bool active() const { return on; } bool busy() const { return on; } };
struct TelegramClient { struct BotWebApp { enum { Closed, Confirm, Loading, Open }; int phase = Closed; }; };
struct Client {
    struct Message { int64_t id; bool outgoing; };
    std::vector<Message> message_rows;
    bool bot_follow = false, bot_focus = false;
    const auto& messages() const { return message_rows; }
    bool followingBotReplies() const { return bot_follow; }
    bool takeBotReplyFocus() { bool focus = bot_focus; bot_focus = false; return focus; }
    struct Folder { int id; std::string title; };
    std::vector<Folder> items;
    int selected = 0;
    const auto& folders() const { return items; }
    int selectedFolder() const { return selected; }
    bool browse = true;
    int chat = 1;
    TelegramClient::BotWebApp app;
    bool canBrowse() const { return browse; }
    bool topicsOpen() const { return false; }
    int currentChatId() const { return chat; }
    auto& botWebApp() { return app; }
} tg;
int main() {
    struct { bool combined_view = true; } settings;
    bool settings_open = false, photo_open = false, video_fullscreen = false;
    bool search_open = false, stickers_open = false, gifs_open = false, reactions_open = false;
    bool seek_open = false, seek_touch_release = false, unsupported_video_prompt = false;
    bool spoiler_prompt = false, bot_panel = false, confirm_leave = false;
    std::string pending_web_link;
    Active upload_picker, circle_recorder, recorder, kb, qr_scanner, compose_wheel;
'''
fixture += '\n'.join(predicates)
start = main.index('        if (combined_visible() && !post_open) {')
end = main.index('        // Separate title,', start)
fixture += r'''
    bool post_open = false, combined_list_focus = true;
    struct { bool open = false; } folder_drawer;
    auto tr = [](const char* value) { return std::string(value); };
    auto header_presence = [&]() {
        std::string presence = "user status";
'''
fixture += main[start:end]
fixture += 'return presence; };\n'

fixture += r'''
    bool* events[] = {&search_open, &stickers_open, &gifs_open, &reactions_open,
        &upload_picker.on, &circle_recorder.on, &recorder.on, &kb.on, &qr_scanner.on,
        &seek_open, &seek_touch_release, &unsupported_video_prompt, &spoiler_prompt,
        &bot_panel, &confirm_leave, &compose_wheel.on};
    const auto base = chat_view::layout(true, true);
    int transitions = 0;
    for (bool combined : {false, true}) for (int chat : {0, 1}) {
        settings.combined_view = combined; tg.chat = chat;
        auto check = [&] {
            assert(chat_modal_open());
            assert(!chat_ui_visible());
            assert(combined_visible() == combined);
            const auto layout = chat_view::layout(combined_visible(), true);
            assert(layout.left() == (combined ? base.left() : 0));
            assert(layout.bubbleWidth() == (combined ? base.bubbleWidth() : 712));
            ++transitions;
        };
        for (auto event : events) {
            *event = true; check();
            // Nested tools, e.g. GIF search/caption keyboard, must keep the same pane.
            for (auto nested : events) if (nested != event) {
                *nested = true; check(); *nested = false; check();
            }
            *event = false;
            assert(!chat_modal_open()); assert(chat_ui_visible());
        }
        for (int phase : {1, 2, 3}) { tg.app.phase = phase; check(); }
        tg.app.phase = 0;
        pending_web_link = "https://example.com"; check(); pending_web_link.clear();
        recorder.on = true; assert(tool_menu_visible());
        kb.on = true; assert(!tool_menu_visible()); kb.on = recorder.on = false;
        upload_picker.on = true; assert(!tool_menu_visible()); upload_picker.on = false;
        for (bool* fullscreen : {&settings_open, &photo_open, &video_fullscreen}) {
            *fullscreen = true; assert(!combined_visible()); *fullscreen = false;
        }
        tg.browse = false; assert(!combined_visible()); tg.browse = true;
    }
    settings.combined_view = true;
    for (bool focus : {false, true}) {
        combined_list_focus = focus;
        const std::string hint = focus ? "chat.focus_list" : "chat.focus_history";
        assert(header_presence() == hint); // No folders loaded yet.
        tg.items = {{0, "Все чаты"}, {4, "Работа"}, {-1, "Архив"}, {9, std::string(200, 'X')}};
        for (int selected : {0, 4, -1, 9, 0, -1, 4}) {
            tg.selected = selected;
            for (bool open : {false, true, false}) {
                folder_drawer.open = open;
                const auto folder = std::find_if(tg.items.begin(), tg.items.end(),
                    [&](const auto& item) { return item.id == selected; });
                assert(header_presence() == (open ? hint : folder->title));
            }
        }
        tg.selected = 123; assert(header_presence() == hint);
        tg.items.clear(); tg.selected = 0;
    }
    tg.items = {{0, "Все чаты"}};
    post_open = true; assert(header_presence() == "user status"); post_open = false;
    settings.combined_view = false; assert(header_presence() == "user status");
    settings.combined_view = true;
    settings_open = true; assert(header_presence() == "user status"); settings_open = false;
    std::cout << "Header folder name: switching, wraparound, archive, long UTF-8 names, drawer and focus: PASS\n";
    using W = chat_view::ToolWindow;
    for (int y = 0; y <= 544; ++y) for (int x = 0; x <= 960; ++x)
        assert(W::backHit(x,y) == (x >= 816 && x < 944 && y >= 38 && y < 64));
    assert(W::left + W::width == 944);
    std::cout << "Modal events/nesting: " << transitions << " transitions; return hit geometry: PASS\n";
'''
start = main.index('        if (was_at_bottom && !post_open && !photo_open')
end = main.index('        if (const int64_t target = tg.takeMessageJump())', start)
fixture += r'''
    int message_index = 0;
    bool was_at_bottom = false;
    int64_t previous_last = 2;
    HistoryScroll history_scroll;
    auto follow_reply = [&] {
'''
fixture += main[start:end]
fixture += r'''
    };
    tg.message_rows = {{1, false}, {2, false}, {3, false}};
    combined_list_focus = true;
    tg.bot_follow = tg.bot_focus = true;
    follow_reply();
    assert(message_index == 2 && !tg.bot_focus && !combined_list_focus);
    auto id = [&](int i) { return tg.message_rows[i].id; };
    auto height = [](int) { return 100; };
    auto inset = [](int) { return 0; };
    history_scroll.configure(3, 180, message_index, id, height, inset);
    assert(history_scroll.first() == 1 && history_scroll.offset() == 20);
    tg.message_rows.push_back({4, false}); was_at_bottom = true; previous_last = 3;
    follow_reply();
    assert(message_index == 3);
    history_scroll.configure(4, 180, message_index, id, height, inset);
    assert(history_scroll.first() == 2 && history_scroll.offset() == 20);
    message_index = 1; was_at_bottom = false; previous_last = 4;
    tg.message_rows.push_back({5, false}); follow_reply();
    assert(message_index == 1); // Reading older history must win over auto-follow.
    was_at_bottom = true; post_open = true; follow_reply(); assert(message_index == 1);
    post_open = false; photo_open = true; follow_reply(); assert(message_index == 1);
    photo_open = false; tg.bot_follow = false; follow_reply(); assert(message_index == 1);
    tg.message_rows.back().outgoing = true; follow_reply(); assert(message_index == 4);
    tg.message_rows.clear(); tg.bot_follow = tg.bot_focus = true; follow_reply();
    assert(message_index == -1 && history_scroll.first() == 0 && history_scroll.offset() == 0);
    tg.message_rows.push_back({6, false}); previous_last = 0; follow_reply();
    assert(message_index == 0);
    std::cout << "Bot reply focus: immediate/latest, incoming replies, viewport, empty history and manual navigation: PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='vitagram-chat-modal-') as tmp:
    source = Path(tmp) / 'modal.cpp'
    binary = Path(tmp) / 'modal'
    source.write_text(fixture)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'src'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
