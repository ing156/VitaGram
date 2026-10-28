"""Exercise the production settings table, touch geometry and menu transitions."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
main = (root / 'src/main.cpp').read_text()

def block(start, after=0):
    pos = main.index(start, after)
    opening = main.index('{', pos)
    depth = 1
    end = opening + 1
    while depth:
        depth += (main[end] == '{') - (main[end] == '}')
        end += 1
    return main[pos:end]

fixture = r'''
#include "settings_menu.h"
#include "chat_list_scroll.h"
#include "ui_layout.h"
#include <cassert>
#include <string>
#include <iostream>
constexpr int SCE_CTRL_CROSS = 1, SCE_CTRL_CIRCLE = 2;
int main() {
    int seen[34]{};
    for (int section = 0; section < 9; ++section) {
        assert(settings_menu::counts[section] > 0 && settings_menu::counts[section] <= 9);
        for (int i = 0; i < settings_menu::counts[section]; ++i) {
            const int source = settings_menu::rows[section][i];
            if (source < 0) {
                assert(section == 0 && -source == i + 1 && -source < 9);
                assert(settings_menu::submenu(source));
            } else { assert(source < 34); ++seen[source]; }
        }
    }
    for (int n : seen) assert(n == 1); // No lost or duplicated setting/action.
    assert(settings_menu::rows[0][8] == 31); // Logout stays outside sections.
    for (int id = 0; id < 34; ++id)
        assert(settings_menu::vaultDependent(id) == (id >= 12 && id <= 15));
    const int kSettingsTop = ui_layout::settings_top;
    const int kSettingsRowHeight = ui_layout::settings_row;
    const int kSettingsViewport = ui_layout::settings_viewport;
    ChatListScroll settings_scroll, settings_root_scroll, settings_parent_scroll;
    int settings_section = 0, settings_row = 0, settings_root_row = 0, settings_parent_row = 0;
    bool settings_open = true, confirm_logout = false, confirm_vault_off = false;
    std::string settings_error;
    int pressed = 0;
'''
for name in ('save_settings_parent', 'restore_settings_parent'):
    fixture += block('auto ' + name + ' = [&]') + ';\n'
fixture += 'auto back = [&] {\n'
start = main.index('        } else if (settings_open) {\n            if (pressed & SCE_CTRL_CIRCLE)')
end = main.index('            const int count =', start)
fixture += main[main.index('            if (pressed', start):end]
fixture += '};\nauto open = [&](int source_row) {\n'
fixture += block('if (source_row < 0)')
fixture += '\n};\n'
# Run the actual child-return call sites, including keyboard's nested language screen.
fixture += r'''
    bool logs_open = true, about_open = true, about_chat_pending = true;
    bool proxy_open = true, proxy_imported = true, proxy_return_to_screen = false;
    int keyboard_settings = 1;
    auto keyboard_rows = [&] { return keyboard_settings == 2 ? 20 : 3; };
    struct Client { void cancelProxyCheck() {} } tg;
'''
for name in ('logs_open', 'about_open', 'proxy_open', 'keyboard_settings'):
    after = main.index('        } else if (settings_open && ' + name + ') {', main.index('bool auth_proxy ='))
    fixture += 'auto return_' + name + ' = [&] {\n' + block('if (pressed & SCE_CTRL_CIRCLE)', after) + '\n};\n'
fixture += r'''
    settings_section = 3; settings_row = 1;
    settings_scroll.configure(4, kSettingsRowHeight, kSettingsViewport);
    save_settings_parent(); pressed = SCE_CTRL_CIRCLE;
    settings_row = 5; return_logs_open(); assert(!logs_open && settings_row == 1);
    settings_row = 5; return_about_open(); assert(!about_open && !about_chat_pending && settings_row == 1);
    settings_row = 5; return_proxy_open(); assert(!proxy_open && !proxy_imported && settings_row == 1);
    keyboard_settings = 2; settings_row = 16; return_keyboard_settings();
    assert(keyboard_settings == 1 && settings_row == 1);
    return_keyboard_settings(); assert(keyboard_settings == 0 && settings_row == 1);
    for (int section = 1; section <= 8; ++section) {
        settings_section = 0; settings_row = section - 1;
        settings_scroll.configure(9, kSettingsRowHeight, kSettingsViewport);
        settings_scroll.drag(-23); // Preserve a partially scrolled parent, not only row visibility.
        const int offset = settings_scroll.offset();
        pressed = SCE_CTRL_CROSS;
        open(-section);
        assert(settings_section == section && settings_row == 0 && settings_scroll.offset() == 0);
        settings_scroll.configure(settings_menu::counts[section], kSettingsRowHeight, kSettingsViewport);
        // Return from a child screen must preserve its parent, too.
        for (int row = 0; row < settings_menu::counts[section]; ++row) {
            if (!settings_menu::submenu(settings_menu::rows[section][row])) continue;
            settings_row = row; settings_scroll.reveal(row);
            const int parent_offset = settings_scroll.offset();
            save_settings_parent();
            settings_row = 17; settings_scroll.configure(25, kSettingsRowHeight, kSettingsViewport);
            settings_scroll.drag(-200); settings_error = "child error";
            restore_settings_parent();
            assert(settings_row == row && settings_scroll.offset() == parent_offset && settings_error.empty());
        }
        pressed = SCE_CTRL_CIRCLE; back();
        assert(settings_section == 0 && settings_row == section - 1);
        assert(settings_scroll.offset() == offset && pressed == 0 && settings_open);
    }
    for (bool* confirmation : {&confirm_logout, &confirm_vault_off}) {
        settings_section = 6; *confirmation = true;
        pressed = SCE_CTRL_CIRCLE; back();
        assert(!*confirmation && settings_section == 6 && settings_open && pressed == 0);
    }
    settings_section = 0; pressed = SCE_CTRL_CIRCLE; back();
    assert(!settings_open && pressed == 0);
    // Every screen pixel, including blank space, gaps and a partially visible last row.
    for (int section = 0; section <= 8; ++section) {
        settings_scroll.configure(settings_menu::counts[section], kSettingsRowHeight, kSettingsViewport);
        settings_scroll.reveal(settings_menu::counts[section] - 1);
        for (int y = 0; y < 544; ++y) for (int x = 0; x < 960; ++x) {
            const int hit = settings_scroll.hit(y - kSettingsTop);
            const int top = kSettingsTop + hit * kSettingsRowHeight - settings_scroll.offset();
            const bool actual = hit >= 0 && settings_menu::rowHit(x, y, top, kSettingsRowHeight);
            const int local = y - kSettingsTop + settings_scroll.offset();
            const bool expected = x >= 20 && x < 940 && y >= kSettingsTop &&
                y < kSettingsTop + kSettingsViewport && local >= 0 &&
                local < settings_menu::counts[section] * kSettingsRowHeight &&
                local % kSettingsRowHeight >= 1 && local % kSettingsRowHeight < kSettingsRowHeight - 1;
            assert(actual == expected);
            assert(settings_menu::backHit(x, y) == (x >= 20 && x < 164 && y >= 34 && y < 64));
        }
    }
    std::cout << "Settings menu: coverage, return navigation and touch bounds PASS\n";
}
'''
# Compile the actual language opener and confirmation handler with a tiny catalog.
fixture = fixture.replace('    std::cout << "Settings menu:', r'''
    {
        struct Settings { std::string language = "ru"; } settings;
        struct Picker {
            int count = 0, focused = 0; bool active = false;
            void open(int index) { focused = index; active = true; }
            void close() { active = false; }
        } theme_picker;
        bool font_picker = true, language_picker = false, language_pending = false;
        std::string requested_language;
        auto language_choice = [](int i) { return i == 0 ? std::string() : i == 1 ? std::string("en") : std::string("ru"); };
        auto prepare = [](const char*) {};
        auto language_font_path_for = [](const std::string&) { return "font"; };
        auto open_language = [&] {
''' + block('else if (source_row == 0)').replace('else if (source_row == 0)', '')
    .replace('(int)i18n::languages().size()', '2') + r'''
        };
        auto choose_language = [&] {
''' + block('else if (theme_picker.active && language_picker && (pressed & SCE_CTRL_CROSS))')
    .replace('else if (theme_picker.active && language_picker && (pressed & SCE_CTRL_CROSS))', '')
    .replace('rich_text_prepare_language_font', 'prepare').replace('i18n::resolve(requested_language)', 'requested_language') + r'''
        };
        open_language();
        assert(language_picker && !font_picker && theme_picker.active && theme_picker.count == 3 && theme_picker.focused == 2);
        theme_picker.focused = 1; theme_picker.close();
        assert(settings.language == "ru" && !language_pending); // Browsing/cancel never applies.
        open_language(); theme_picker.focused = 0; choose_language();
        assert(requested_language.empty() && language_pending && !theme_picker.active && settings.language == "ru");
        language_pending = false; settings.language = "missing";
        open_language(); assert(theme_picker.focused == 0);
        theme_picker.focused = 1; choose_language();
        assert(requested_language == "en" && language_pending && !theme_picker.active);
    }
    std::cout << "Settings menu:''')
with tempfile.TemporaryDirectory(prefix='vitagram-settings-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(fixture)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'src'), str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
