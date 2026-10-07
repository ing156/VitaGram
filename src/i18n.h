#pragma once
#include <initializer_list>
#include <string>
#include <vector>

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

const std::vector<Language>& languages();
// Code of the language currently in use (e.g. "en").
std::string current();
// Code the system language maps to ("" when unknown).
std::string systemLanguage();

std::string tr(const char* key);
std::string tr(const char* key, std::initializer_list<std::string> args);

}  // namespace i18n

using i18n::tr;
