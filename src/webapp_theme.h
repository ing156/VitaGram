#pragma once
#include "ui_skin.h"
#include "ui_color.h"
#include <jansson.h>
#include <string>
#include <cstdio>
#include <cstdlib>

inline json_t* webapp_theme(bool dark) {
    dark = ui_skin::is_dark(dark);
    const unsigned surface = ui_skin::surface_color(ui_skin::color("surface", dark ? 0xff3b2d1f : 0xffffffff)) | 0xff000000u;
    const unsigned ink = ui_color::tone(ui_skin::color("text", dark ? 0xfff5eee7 : 0xff423327), surface, surface, 4.5);
    const unsigned accent = ui_color::tone(ui_skin::color("accent", 0xffd28723), surface, surface, 4.5);
    const unsigned button = ui_skin::color("selected", dark ? 0xff5e482e : 0xfff9e8cf) | 0xff000000u;
    auto rgb = [](unsigned abgr) { return int((abgr & 255) << 16 | (abgr & 0xff00) | ((abgr >> 16) & 255)); };
    auto* theme = json_pack("{s:s}", "@type", "themeParameters");
    for (const char* key : {"background_color", "secondary_background_color", "header_background_color", "bottom_bar_background_color", "section_background_color"})
        json_object_set_new(theme, key, json_integer(rgb(surface)));
    for (const char* key : {"text_color", "subtitle_text_color", "hint_color"}) json_object_set_new(theme, key, json_integer(rgb(ink)));
    for (const char* key : {"accent_text_color", "section_header_text_color", "link_color", "destructive_text_color"}) json_object_set_new(theme, key, json_integer(rgb(accent)));
    json_object_set_new(theme, "section_separator_color", json_integer(rgb(ui_skin::color("divider", button))));
    json_object_set_new(theme, "button_color", json_integer(rgb(button)));
    json_object_set_new(theme, "button_text_color", json_integer(rgb(ui_color::on(button))));
    return theme;
}

inline std::string webapp_theme_event(json_t* theme) {
    auto* params = json_object();
    const char* fields[][2] = {{"background_color", "bg_color"}, {"secondary_background_color", "secondary_bg_color"},
        {"header_background_color", "header_bg_color"}, {"bottom_bar_background_color", "bottom_bar_bg_color"},
        {"section_background_color", "section_bg_color"}, {"section_separator_color", "section_separator_color"},
        {"text_color", "text_color"}, {"accent_text_color", "accent_text_color"}, {"section_header_text_color", "section_header_text_color"},
        {"subtitle_text_color", "subtitle_text_color"}, {"destructive_text_color", "destructive_text_color"}, {"hint_color", "hint_color"},
        {"link_color", "link_color"}, {"button_color", "button_color"}, {"button_text_color", "button_text_color"}};
    for (const auto& field : fields) {
        char value[8]; std::snprintf(value, sizeof(value), "#%06x", (unsigned)json_integer_value(json_object_get(theme, field[0])) & 0xffffff);
        json_object_set_new(params, field[1], json_string(value));
    }
    auto* event = json_pack("{s:o}", "theme_params", params); auto* bytes = json_dumps(event, JSON_COMPACT);
    std::string result = bytes ? bytes : "{}"; free(bytes); json_decref(event); return result;
}
