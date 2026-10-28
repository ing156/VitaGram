#pragma once
#include <string>
#include <vector>
#include <map>

namespace keyboard_layout {
struct Layout {
    std::string code, badge, name;
    std::vector<std::string> rows;
    std::map<std::string, std::vector<std::string>> popups;
    std::map<std::string, std::string> uppercase;
    std::vector<std::string> digits;
    size_t digit_start = 0;
    std::vector<std::string> symbols;
    std::map<std::string, std::string> shifted;
    bool system() const { return rows.empty(); }
};
const std::vector<Layout>& all();
const Layout& get(const std::string& code);
std::string upper(const std::string& text, const std::string& code);
std::vector<std::string> characters(const std::string& text);
std::vector<std::string> alternatives(const Layout& layout, const std::string& label,
                                     size_t row, size_t column, bool shifted);
}
