#pragma once
#include <initializer_list>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

// Localization. Texts live in plain files (assets/lang/<code>.lang, packaged as
// app0:lang/<code>.lang); adding a language means adding one file, no code changes.
//
// File format (UTF-8, one entry per line):
//   # comment
//   @code = ru            language code (defaults to the file name)
//   @name = Русский       name shown in the language menu, in that language
//   some.key = Text       "\n" is a line break, "\\" a backslash
//
// Inside a text:
//   {0} {1} ...           arguments passed to tr()
//   {cross} {circle} ...  controller button icons; they are substituted when the file
//                         is loaded, so a translation never contains the icon glyphs
//                         themselves and cannot change how they are drawn.
//
// Lookup order: selected language -> English -> the key itself.
namespace i18n {

struct Language {
    std::string code;
    std::string name;
};

// Scans `directory` for *.lang files and selects the language: `preference` (a code
// stored in settings) or, when it is empty ("auto"), the console system language.
void init(const std::string& directory, const std::string& preference);
// Empty preference means automatic. Returns false when the code is unknown.
bool select(const std::string& preference);
// Resolve a preference without changing the active catalog.
std::string resolve(const std::string& preference);

const std::vector<Language>& languages();
// Code of the language currently in use (e.g. "en").
std::string current();
// Code the system language maps to ("" when unknown).
std::string systemLanguage();

std::string tr(const char* key);
std::string tr(const char* key, std::initializer_list<std::string> args);

// Persistent UI text keeps its key/arguments, never tries to reverse-translate
// a rendered string. Plain user/server strings have no translation recipe.
uint64_t revision();
class Text : public std::string {
public:
    Text() = default;
    Text(const char* value) : std::string(value) {}
    Text(const std::string& value) : std::string(value) {}
    Text(std::string&& value) : std::string(std::move(value)) {}
    Text(const Text&) = default;
    Text(Text&&) = default;
    Text& operator=(const Text&) = default;
    Text& operator=(Text&&) = default;
    Text& operator=(const std::string& value) { return *this = Text(value); }
    Text& operator=(std::string&& value) { return *this = Text(std::move(value)); }
    Text& operator=(const char* value) { return *this = Text(value); }
    void clear() { std::string::clear(); render_.reset(); }
    std::string localized() const;
    void refresh();
    Text& operator+=(const Text& value);
    Text& operator+=(const std::string& value) { return *this += Text(value); }
    Text& operator+=(const char* value) { return *this += Text(value); }
    Text& operator+=(char value) { return *this += Text(std::string(1, value)); }
    void replaceTranslation(const Text& needle, const Text& replacement);
    static Text computed(std::function<std::string()> render);
private:
    friend Text operator+(const Text& left, const Text& right);
    std::shared_ptr<const std::function<std::string()>> render_;
    uint64_t revision_ = 0;
};
Text text(const char* key, std::initializer_list<Text> args = {});
Text operator+(const Text& left, const Text& right);
Text operator+(const Text& left, const std::string& right);
Text operator+(const std::string& left, const Text& right);
Text operator+(const Text& left, std::string&& right);
Text operator+(std::string&& left, const Text& right);
Text operator+(const Text& left, const char* right);
Text operator+(const char* left, const Text& right);

}  // namespace i18n

using i18n::tr;
