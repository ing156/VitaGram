#pragma once
#include <psp2/ime_dialog.h>
#include <string>

struct vita2d_pgf;

class Keyboard {
public:
    Keyboard();
    void open(const std::string& title, bool secret = false, const std::string& initial = "");
    void close();
    void clear(); // overwrites the typed text (passphrases)
    bool active() const { return active_; }
    bool submitted() const { return submitted_; }
    bool cancelled() const { return cancelled_; }
    const std::string& text() const { return text_; }
    void handle(unsigned int pressed);
    void draw(vita2d_pgf* font) const;
private:
    static constexpr unsigned int MaxTextLength = 1023;
    bool active_ = false, submitted_ = false, cancelled_ = false;
    std::string text_;
    SceImeDialogParam param_{};
    unsigned short title_[128]{}, initial_[MaxTextLength + 1]{}, input_[MaxTextLength + 1]{};
};
