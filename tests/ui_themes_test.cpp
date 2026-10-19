#include "ui_themes.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

int main() {
    assert(ui_themes::themes.size()==7);
    assert(std::string(ui_themes::get(5).palette)=="black-blue/palette.json");
    assert(std::string(ui_themes::get(6).palette)=="black-red/palette.json");
    for(int id=0;id<7;++id)assert(ui_themes::from_saved_id(ui_themes::saved_id(id))==id);
    assert(ui_themes::from_saved_id(6)==5 && ui_themes::from_saved_id(7)==6);
    for(int id:{5,8,9,10,11,12,13,14,15,2147483647})assert(ui_themes::from_saved_id(id)==1);
    char pattern[] = "/tmp/vitagram-themes-XXXXXX";
    const char* work = mkdtemp(pattern);
    assert(work);
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(work);
    std::filesystem::create_directories(ui_themes::directory);
    const std::string valid = R"({"background":"#080609","surface":"#160E14","text":"#FFF3F6","muted":"#DABFC9","accent":"#FF365D"})";
    auto write = [&](const std::string& name, const std::string& text) {
        std::ofstream(std::string(ui_themes::directory) + '/' + name) << text;
    };
    write("Красная ночь.json", valid);
    write("Blue.JSON", valid.substr(0,valid.size()-1) + ",\"delivery\":\"#112233\"}");
    write("Unknown.json", R"({"oops":"#FFFFFF"})");
    write("Duplicate.json", R"({"background":"#000000","background":"#FFFFFF"})");
    write("Partial.json", R"({"surface":"#FFFFFF"})");
    write("Broken.json", "{");
    write("Big.json", std::string(8193, ' '));
    write(".Hidden.json", valid);
    write("Wrong.txt", valid);
    std::filesystem::create_directories(std::string(ui_themes::directory) + "/Directory.json");
    std::filesystem::create_symlink("Blue.JSON", std::string(ui_themes::directory) + "/Link.json");
    assert(ui_themes::valid_filename(std::string(96, 'a') + ".json"));
    for (const auto& bad : std::vector<std::string>{"../Outside.json", "Bad?.json", ".json", "Space .json", "Dot..json", " Bad.json",
                            std::string(97, 'a') + ".json", std::string("\xFF.json")})
        assert(!ui_themes::valid_filename(bad));
    const auto entries = ui_themes::scan_custom();
    assert(entries.size() == 2 && entries[0].filename == "Blue.JSON" && entries[1].filename == "Красная ночь.json");
    assert(entries[0].base == 1 && entries[0].fields & (1u << ui_mask::role("accent")));
    assert(entries[0].fields & (uint64_t(1) << ui_mask::role("control_inactive")));
    assert(entries[0].fields & (uint64_t(1) << ui_mask::role("delivery")));
    assert(entries[0].colors[ui_mask::role("delivery")] == 0xFF332211u);
    assert(ui_themes::count() == (int)ui_themes::themes.size() + 2);
    const int selected = ui_themes::find_file("Красная ночь.json");
    assert(ui_themes::custom(selected)->filename == "Красная ночь.json");
    assert(ui_themes::get(selected).base == 1 && ui_themes::find_file("Deleted.json") == 1);
    write("A New.json", valid);
    assert(ui_themes::find_file("Красная ночь.json") == selected); // Session IDs stay stable until restart.
    assert(ui_themes::scan_custom()[0].filename == "A New.json");
    for (int i = 0; i < 70; ++i) write("Theme " + std::to_string(i) + ".json", valid);
    assert(ui_themes::scan_custom().size() == ui_themes::max_custom);
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(work);
    std::cout << "Custom themes: strict bounded files, UTF-8 names, ordering, limits and stable session IDs passed\n";
}
