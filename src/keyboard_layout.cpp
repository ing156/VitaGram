#include "keyboard_layout.h"
#include <algorithm>

namespace keyboard_layout {
// Standard character relationships checked against HeliBoard locale_key_texts and layouts.
// https://github.com/HeliBorg/HeliBoard/tree/main/app/src/main/assets/locale_key_texts
// Independent layout model and input implementation; no Android implementation is included.
const std::vector<Layout>& all() {
    static const std::vector<Layout> layouts = {
        {"en", "EN", "English", {"qwertyuiop", "asdfghjkl", "zxcvbnm"}, {{"a", {"à", "á", "â", "ä", "æ", "ã", "å", "ā"}}, {"e", {"é", "è", "ê", "ë", "ē"}}, {"i", {"í", "î", "ï", "ī", "ì"}}, {"o", {"ó", "ô", "ö", "ò", "œ", "ø", "ō", "õ"}}, {"u", {"ú", "û", "ü", "ù", "ū"}}, {"s", {"ß"}}, {"n", {"ñ"}}, {"c", {"ç"}}}},
        {"ru", "RU", "Русский", {"йцукенгшщзх", "фывапролджэ", "ячсмитьбю"}, {{"е", {"ё"}}, {"ь", {"ъ"}}}},
        {"de", "DE", "Deutsch", {"qwertzuiop", "asdfghjkl", "yxcvbnm"}, {{"a", {"ä"}}, {"o", {"ö"}}, {"u", {"ü"}}, {"s", {"ß"}}, {"'", {"’", "‚", "‘", "›", "‹"}}, {"\"", {"”", "„", "“", "»", "«"}}}},
        {"fr", "FR", "Français", {"azertyuiop", "qsdfghjklm", "wxcvbn'"}, {{"a", {"à", "â", "æ"}}, {"e", {"é", "è", "ê", "ë"}}, {"i", {"î", "ï"}}, {"o", {"ô", "œ"}}, {"u", {"ù", "û", "ü"}}, {"c", {"ç"}}, {"y", {"ÿ"}}}},
        {"es", "ES", "Español", {"qwertyuiop", "asdfghjklñ", "zxcvbnm"}, {{"a", {"á", "ª"}}, {"e", {"é"}}, {"i", {"í"}}, {"o", {"ó", "º"}}, {"u", {"ú", "ü"}}, {"n", {"ñ"}}, {"y", {"ý"}}}},
        {"it", "IT", "Italiano", {"qwertyuiop", "asdfghjkl", "zxcvbnm"}, {{"a", {"à", "ª"}}, {"e", {"è", "é", "ə", "ɜ"}}, {"i", {"ì"}}, {"o", {"ò", "ó", "º"}}, {"u", {"ù"}}}},
        {"nl", "NL", "Nederlands", {"qwertyuiop", "asdfghjkl", "zxcvbnm"}, {{"a", {"á", "ä", "â", "à"}}, {"e", {"é", "ë", "ê", "è"}}, {"i", {"í", "ï", "ì", "î", "ĳ"}}, {"o", {"ó", "ö", "ô", "ò"}}, {"u", {"ú", "ü", "û", "ù"}}, {"y", {"ý", "ÿ"}}, {"'", {"‘", "‚", "’"}}, {"\"", {"“", "„", "”"}}}},
        {"pt-PT", "PT", "Português (PT)", {"qwertyuiop", "asdfghjkl", "zxcvbnm"}, {{"a", {"á", "ã", "à", "â", "ª"}}, {"e", {"é", "ê"}}, {"i", {"í"}}, {"o", {"ó", "õ", "ô", "º"}}, {"u", {"ú", "ü"}}, {"c", {"ç"}}}},
        {"pt-BR", "BR", "Português (BR)", {"qwertyuiop", "asdfghjkl", "zxcvbnm"}, {{"a", {"á", "ã", "à", "â", "ª"}}, {"e", {"é", "ê"}}, {"i", {"í"}}, {"o", {"ó", "õ", "ô", "º"}}, {"u", {"ú", "ü"}}, {"c", {"ç"}}}},
        {"sv", "SV", "Svenska", {"qwertyuiopå", "asdfghjklöä", "zxcvbnm"}, {{"a", {"ä", "å"}}, {"o", {"ö"}}, {"'", {"›", "‹"}}, {"\"", {"»", "«"}}}},
        {"da", "DA", "Dansk", {"qwertyuiopå", "asdfghjklæø", "zxcvbnm"}, {{"a", {"å", "æ"}}, {"e", {"é"}}, {"o", {"ø"}}, {"'", {"’", "‚", "‘", "›", "‹"}}, {"\"", {"”", "„", "“", "»", "«"}}}},
        {"no", "NO", "Norsk", {"qwertyuiopå", "asdfghjkløæ", "zxcvbnm"}, {{"a", {"å", "æ"}}, {"e", {"é"}}, {"o", {"ø"}}, {"'", {"‘", "‚", "’"}}, {"\"", {"”", "„", "“", "«", "»"}}}},
        {"fi", "FI", "Suomi", {"qwertyuiopå", "asdfghjklöä", "zxcvbnm"}, {{"a", {"ä", "å"}}, {"o", {"ö"}}, {"s", {"š"}}, {"z", {"ž"}}}},
        {"pl", "PL", "Polski", {"qwertyuiop", "asdfghjkl", "zxcvbnm"}, {{"a", {"ą"}}, {"e", {"ę"}}, {"o", {"ó"}}, {"s", {"ś"}}, {"n", {"ń"}}, {"c", {"ć"}}, {"z", {"ż", "ź"}}, {"l", {"ł"}}, {"'", {"‘", "‚", "’"}}, {"\"", {"“", "„", "”"}}}},
        {"tr", "TR", "Türkçe", {"qwertyuıopğü", "asdfghjklşi", "zxcvbnmöç"}, {{"a", {"â"}}, {"ı", {"i", "î"}}, {"i", {"ı", "î"}}, {"o", {"ö"}}, {"u", {"ü", "û"}}, {"s", {"ş"}}, {"g", {"ğ"}}, {"c", {"ç"}}}},
        {"ja", "JA", "JA", {}, {}},
        {"ko", "KO", "KO", {}, {}},
        {"zh-Hans", "ZH-CN", "ZH-CN", {}, {}},
        {"zh-Hant", "ZH-TW", "ZH-TW", {}, {}}
    };
    return layouts;
}
const Layout& get(const std::string& code) {
    for (const auto& layout : all()) if (layout.code == code) return layout;
    return all().front();
}
std::vector<std::string> characters(const std::string& text) {
    std::vector<std::string> result;
    for (size_t i = 0; i < text.size();) {
        size_t n = 1;
        while (i+n < text.size() && (static_cast<unsigned char>(text[i+n]) & 0xc0) == 0x80) ++n;
        result.push_back(text.substr(i,n)); i += n;
    }
    return result;
}
std::string upper(const std::string& text, const std::string& code) {
    static const std::map<std::string, std::string> pairs = {
        {"a", "A"}, {"b", "B"}, {"c", "C"}, {"d", "D"}, {"e", "E"}, {"f", "F"}, {"g", "G"}, {"h", "H"}, {"i", "I"}, {"j", "J"}, {"k", "K"}, {"l", "L"}, {"m", "M"}, {"n", "N"}, {"o", "O"}, {"p", "P"}, {"q", "Q"}, {"r", "R"}, {"s", "S"}, {"t", "T"}, {"u", "U"}, {"v", "V"}, {"w", "W"}, {"x", "X"}, {"y", "Y"}, {"z", "Z"}, {"ß", "ẞ"}, {"à", "À"}, {"á", "Á"}, {"â", "Â"}, {"ã", "Ã"}, {"ä", "Ä"}, {"å", "Å"}, {"æ", "Æ"}, {"ç", "Ç"}, {"è", "È"}, {"é", "É"}, {"ê", "Ê"}, {"ë", "Ë"}, {"ì", "Ì"}, {"í", "Í"}, {"î", "Î"}, {"ï", "Ï"}, {"ñ", "Ñ"}, {"ò", "Ò"}, {"ó", "Ó"}, {"ô", "Ô"}, {"õ", "Õ"}, {"ö", "Ö"}, {"ø", "Ø"}, {"ù", "Ù"}, {"ú", "Ú"}, {"û", "Û"}, {"ü", "Ü"}, {"ý", "Ý"}, {"ÿ", "Ÿ"}, {"ā", "Ā"}, {"ą", "Ą"}, {"ć", "Ć"}, {"ē", "Ē"}, {"ę", "Ę"}, {"ğ", "Ğ"}, {"ī", "Ī"}, {"ı", "I"}, {"ĳ", "Ĳ"}, {"ł", "Ł"}, {"ń", "Ń"}, {"ō", "Ō"}, {"œ", "Œ"}, {"ś", "Ś"}, {"ş", "Ş"}, {"š", "Š"}, {"ū", "Ū"}, {"ź", "Ź"}, {"ż", "Ż"}, {"ž", "Ž"}, {"ə", "Ə"}, {"ɜ", "Ɜ"}, {"а", "А"}, {"б", "Б"}, {"в", "В"}, {"г", "Г"}, {"д", "Д"}, {"е", "Е"}, {"ж", "Ж"}, {"з", "З"}, {"и", "И"}, {"й", "Й"}, {"к", "К"}, {"л", "Л"}, {"м", "М"}, {"н", "Н"}, {"о", "О"}, {"п", "П"}, {"р", "Р"}, {"с", "С"}, {"т", "Т"}, {"у", "У"}, {"ф", "Ф"}, {"х", "Х"}, {"ц", "Ц"}, {"ч", "Ч"}, {"ш", "Ш"}, {"щ", "Щ"}, {"ъ", "Ъ"}, {"ы", "Ы"}, {"ь", "Ь"}, {"э", "Э"}, {"ю", "Ю"}, {"я", "Я"}, {"ё", "Ё"}
    };
    std::string result;
    for (const auto& c : characters(text)) {
        if (code == "tr" && c == "i") result += "İ";
        else if (code == "tr" && c == "ı") result += "I";
        else { const auto found = pairs.find(c); result += found == pairs.end() ? c : found->second; }
    }
    return result;
}
std::vector<std::string> alternatives(const Layout& layout, const std::string& label,
                                     size_t row, size_t column, bool shifted) {
    std::vector<std::string> result;
    const auto found = layout.popups.find(label);
    if (found != layout.popups.end())
        for (const auto& option : found->second) result.push_back(shifted ? upper(option, layout.code) : option);
    const bool russian = layout.code == "ru";
    if (row == 0 && column >= (russian ? 1u : 0u) && column < (russian ? 11u : 10u)) {
        const std::string digit = std::to_string((column + (russian ? 0 : 1)) % 10);
        if (std::find(result.begin(), result.end(), digit) == result.end()) result.push_back(digit);
    }
    return result;
}
}
