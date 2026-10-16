#pragma once
#include <psp2/ime_dialog.h>
#include <string>
#include "custom_keyboard.h"

struct vita2d_pgf;

class Keyboard {
public:
    Keyboard();
    void configure(bool custom, bool dark, const std::vector<std::string>& languages = {});
    bool systemActive() const { return active_ && !custom_active_; }
    void open(const std::string& title, bool secret = false, const std::string& initial = "", bool message = false);
    void close();
    void clear(); // overwrites the typed text (passphrases)
    bool active() const { return active_; }
    bool submitted() const { return submitted_; }
    bool cancelled() const { return cancelled_; }
    const std::string& text() const { return text_; }
    void handle(unsigned int pressed, unsigned int held, bool touching, int x, int y, uint64_t now);
    void draw(vita2d_pgf* font) const;
private:
    void openSystem(const std::string& title, bool secret, const std::string& initial, const std::string& language);
    CustomKeyboard custom_;
    std::vector<std::string> languages_;
    bool secret_ = false, message_ = false;
    bool prefer_custom_ = false, custom_active_ = false, dark_ = true, suppress_opening_input_ = false;
    std::string caption_;
    static constexpr unsigned int MaxTextLength = 1023;
    bool active_ = false, submitted_ = false, cancelled_ = false;
    std::string text_;
    SceImeDialogParam param_{};
    unsigned short title_[128]{}, initial_[MaxTextLength + 1]{}, input_[MaxTextLength + 1]{};
};
