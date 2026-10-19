#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
struct vita2d_pgf;
struct SceTouchData;

// Shared on-screen keyboard for vault secrets and ordinary text. No system IME calls.
class CustomKeyboard {
public:
    enum class Result { None, Submitted, Cancelled, System };
    CustomKeyboard();
    ~CustomKeyboard();
    void open(const std::string& initial, bool secret, bool message, size_t max_bytes, bool allow_system = false, bool auto_capitalize = true);
    void clear();
    void setLanguages(const std::vector<std::string>& codes);
    bool setLanguage(const std::string& code);
    const std::string& language() const;
    bool systemRequested() const;
    Result handle(unsigned pressed, unsigned held, bool touch_down, int x, int y, uint64_t now, const SceTouchData* touches = nullptr);
    void draw(vita2d_pgf* font, bool dark, const std::string& title, bool keyboard_only = false) const;
    const std::string& text() const;
    bool split() const;
    size_t cursor() const; // UTF-8 byte offset.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
