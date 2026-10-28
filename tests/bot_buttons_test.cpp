#include "bot_buttons.h"
#include "ui_color.h"
#include "utf8_decode.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cmath>
#include <cassert>
#include <cstdio>

int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace bot_buttons;
    assert(toolbar_hit(true, false, 100, 520));
    assert(!toolbar_hit(false, true, 100, 520));
    assert(toolbar_hit(false, true, 400, 520));
    for (int x : {79, 290, 300, 590, 600, 900}) assert(!toolbar_hit(true, true, x, 520));
    for (int y : {497, 542}) assert(!toolbar_hit(true, true, 100, y));
    for (int left : {0, 224}) {
        const PanelGeometry geometry {left};
        assert(geometry.x(0) == left && geometry.x(960) == 960);
        const auto menu = geometry.cell(menu_control), keyboard = geometry.cell(keyboard_control);
        assert(menu.x >= left && keyboard.x >= menu.x + menu.w);
        for (int y = 0; y < 544; ++y) for (int x = 0; x < 960; ++x) {
            const bool visible = menu.hit(x, y) || keyboard.hit(x, y);
            assert(toolbar_hit(true, true, x, y, left) == visible);
            if (x < left) assert(!visible);
        }
        for (int total : {0, 1, 4, 5, 17}) for (int focus = 0; focus < std::max(1, total); ++focus)
            for (int64_t elapsed : {int64_t(0), sheet_duration / 2, sheet_duration}) {
                const auto sheet = command_sheet(total, focus, elapsed, false, left);
                for (const auto& cell : sheet.cells()) {
                    assert(cell.x >= left && cell.x + cell.w <= 960 && cell.w > 0);
                    assert(cell.hit(cell.x, cell.y) && cell.hit(cell.x + cell.w - 1, cell.y + cell.h - 1));
                    assert(!cell.hit(cell.x - 1, cell.y) && !cell.hit(cell.x + cell.w, cell.y));
                }
                assert(sheet.back().x >= left && sheet.back().x + sheet.back().w <= 960);
            }
        const auto last = command_sheet(5, 4, sheet_duration, false, left);
        const auto cell = last.cells().front();
        for (int y = 36; y < 544; ++y) for (int x = 0; x < 960; ++x) {
            const bool inside = x >= cell.x && x < cell.x + cell.w && y >= cell.y && y < cell.y + cell.h;
            assert(cell.hit(x, y) == inside);
            if (x < left) assert(!cell.hit(x, y));
        }
        for (int x : {200, 390, 740}) {
            const int w = x == 740 ? 172 : 165;
            const auto control = geometry.cell({0, 0, x, 474, w, 26});
            assert(control.w == geometry.span(x, w));
        }
    }
    for (int total : {0, 1, 4, 5, 17}) for (int focus = 0; focus < std::max(1, total); ++focus) {
        const auto sheet = command_sheet(total, focus, sheet_duration, false);
        const auto opening = command_sheet(total, focus, 0, false);
        const auto closing = command_sheet(total, focus, sheet_duration, true);
        assert(sheet.offset == 0 && opening.top + opening.offset == 544);
        assert(closing.offset == opening.offset);
        assert(command_sheet(total, focus, -1, false).offset == opening.offset);
        assert(command_sheet(total, focus, sheet_duration * 2, false).offset == 0);
        assert(sheet.count == std::min(4, total - sheet.first));
        assert(sheet.cells().size() == (size_t)sheet.count);
        for (const auto& cell : sheet.cells()) {
            assert(cell.row >= sheet.first && cell.row < sheet.first + sheet.count && cell.column == 0);
            assert(cell.hit(cell.x, cell.y) && cell.hit(cell.x + cell.w - 1, cell.y + cell.h - 1));
            assert(!cell.hit(cell.x - 1, cell.y) && !cell.hit(cell.x + cell.w, cell.y));
            assert(!cell.hit(cell.x, cell.y + cell.h));
            assert(cell.y >= sheet.top + 44 && cell.y + cell.h <= 472);
        }
        int previous = opening.offset;
        for (int64_t elapsed = 0; elapsed <= sheet_duration; elapsed += 10000) {
            const auto animated = command_sheet(total, focus, elapsed, false);
            assert(animated.offset <= previous); previous = animated.offset;
            for (const auto& cell : animated.cells())
                assert(cell.y == sheet.cells()[cell.row - sheet.first].y + animated.offset);
            assert(animated.back().y == 502 + animated.offset);
        }
    }
    const auto last_sheet = command_sheet(5, 4, sheet_duration, false);
    const auto last_cells = last_sheet.cells();
    assert(last_sheet.first == 4 && last_sheet.count == 1);
    for (int y = 36; y < 544; ++y) for (int x = 0; x < 960; ++x) {
        int hits = 0; for (const auto& cell : last_cells) hits += cell.hit(x, y);
        assert(hits <= 1);
        if (y < last_sheet.top + 44 || y >= last_sheet.top + 100 || x < 32 || x >= 928) assert(!hits);
    }
    Markup app_menu; app_menu.rows = {{{"Open", "inlineKeyboardButtonTypeWebApp", "https://example.org"}}};
    assert(menu_app(app_menu));
    app_menu.rows.push_back({{"/help", "command", "/help"}}); assert(!menu_app(app_menu));
    assert(parse(nullptr).empty());
    json_error_t error{};
    auto* json = json_loads(R"({"@type":"replyMarkupShowKeyboard","rows":[[],[{"text":"👨‍👩‍👧‍👦","type":{"@type":"keyboardButtonTypeText"}},{"text":"Two","type":{"@type":"keyboardButtonTypeRequestPhoneNumber"}}],[{"text":"Three","type":{"@type":"keyboardButtonTypeWebApp","url":"https://example.org"}}]]})", 0, &error);
    assert(json);
    auto markup = parse(json); json_decref(json);
    assert(size(markup) == 3 && markup.rows.size() == 2);
    assert(markup.rows[0][0].label == "👨‍👩‍👧‍👦");
    assert(markup.rows[0][1].type == "keyboardButtonTypeRequestPhoneNumber");
    assert(markup.rows[1][0].value == "https://example.org");
    auto grid = cells(markup, 142, 44);
    assert(grid.size() == 3 && grid.back().w == 864);
    for (const auto& cell : grid) {
        assert(cell.hit(cell.x, cell.y));
        assert(cell.hit(cell.x + cell.w - 1, cell.y + cell.h - 1));
        assert(!cell.hit(cell.x + cell.w, cell.y));
        assert(!cell.hit(cell.x, cell.y + cell.h));
    }
    for (int y = 36; y < 544; ++y) for (int x = 0; x < 960; ++x) {
        int hits = 0;
        for (const auto& cell : grid) hits += cell.hit(x, y);
        assert(hits <= 1);
        if (x < 48 || x >= 912 || y < 142 || y >= 240) assert(!hits);
        if (x >= 474 && x < 486 && y < 186) assert(!hits);
    }
    // Odd source rows and page boundaries leave real gaps, never phantom items.
    markup.rows = {{{"a", "", ""}, {"b", "", ""}, {"c", "", ""}},
                   {{"d", "", ""}}, {{"e", "", ""}}, {{"f", "", ""}}, {{"g", "", ""}}};
    for (int focus = 0; focus < size(markup); ++focus) {
        const auto p = page(markup, focus, 2);
        assert(p.first <= focus && focus < p.first + p.count && p.total == 3);
        const auto visible = cells(markup, 358, 44, p.first, p.count);
        assert(visible.size() == (size_t)p.count && visible.back().y + visible.back().h <= 456);
        for (const auto& cell : visible) assert(cell.row >= 0 && cell.column < (int)markup.rows[cell.row].size());
    }
    assert(cells(markup, 142, 44, 6).size() == 1);
    assert(cells(markup, 142, 44, 12).empty());
    markup.rows.clear();
    for (int i = 0; i < 6; ++i) markup.rows.push_back({{"long label", "", ""}});
    grid = cells(markup, 142, 44);
    assert(grid.back().y + grid.back().h < 480);
    // Dark, light and arbitrary custom surfaces keep text >= 4.5:1 and focus >= 3:1.
    for (unsigned color : {0xff102030u, 0xfff0eee8u, 0xff808080u, 0xff0000ffu, 0xff40a060u})
        for (unsigned requested : {0xff000000u, 0xffffffffu, 0xff808080u, 0xff007accu}) {
            assert(ui_color::contrast(ui_color::tone(requested, color, color, 4.5), color) >= 4.5);
            assert(ui_color::contrast(ui_color::tone(requested, color, color, 3.0), color) >= 3.0);
        }
    FT_Library library{}; assert(!FT_Init_FreeType(&library));
    const char* fonts[] = {"NotoSans-Regular.ttf", "NotoSansJP-Regular.otf", "NotoSansKR-Regular.otf", "NotoSansSC-Regular.otf", "NotoSansTC-Regular.otf"};
    const char* samples[] = {"Меню Новости Помощь Скачать gyjp Åéöç Ąę", "メニューキーボード", "봇 키보드 버튼", "机器人回复", "機器人回覆"};
    for (int f = 0; f < 5; ++f) {
        FT_Face face{};
        assert(!FT_New_Face(library, (std::string(argv[1]) + "/" + fonts[f]).c_str(), 0, &face));
        for (int setting = 0; setting <= 6; ++setting) {
            const float scale = 0.7f + setting * 0.1f;
            for (auto layout : {std::pair<float,int>{0.62f, 44}, {0.55f, 36}, {0.62f, 56}}) {
                const int baseline = layout.second == 44 ? 29 : layout.second == 56 ? 35 : 24;
                assert(!FT_Set_Pixel_Sizes(face, 0, (unsigned)std::lround(32 * layout.first * scale)));
                for (const char* p = samples[f]; *p;) {
                    uint32_t code = 0; p += decode_utf8_scalar(p, code);
                    assert(!FT_Load_Char(face, code, FT_LOAD_RENDER));
                    const auto& glyph = *face->glyph;
                    assert(baseline - glyph.bitmap_top >= 0);
                    assert(baseline - glyph.bitmap_top + (int)glyph.bitmap.rows <= layout.second);
                }
            }
        }
        FT_Done_Face(face);
    }
    FT_Done_FreeType(library);
    std::puts("Bot buttons: parser, emoji, touch geometry, pages, contrast and real glyph bounds at all seven font sizes passed");
}
