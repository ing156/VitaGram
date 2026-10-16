#pragma once
#include <cstdint>
#include <string>

// Case state is independent of focus, touch previews and long-press choices.
class KeyboardCase {
public:
    static constexpr uint64_t doubleTapDelay = 350000;
    void reset(bool automatic) { automatic_ = automatic; mode_ = Mode::Auto; pending_tap_ = false; }
    bool upper(const std::string& text) const {
        return mode_ == Mode::Caps || mode_ == Mode::Once ||
            (mode_ == Mode::Auto && automatic_ && sentenceStart(text));
    }
    bool explicitUpper() const { return mode_ == Mode::Once || mode_ == Mode::Caps; }
    bool caps() const { return mode_ == Mode::Caps; }
    void shift(uint64_t now, const std::string& text) {
        if (mode_ == Mode::Caps) { mode_ = Mode::Lower; pending_tap_ = false; return; }
        if (pending_tap_ && now >= last_tap_ && now - last_tap_ <= doubleTapDelay) {
            mode_ = Mode::Caps; pending_tap_ = false; return;
        }
        mode_ = upper(text) ? Mode::Lower : Mode::Once;
        last_tap_ = now; pending_tap_ = true;
    }
    // Only a successful character insertion spends one-shot Shift; space retains it.
    void inserted() { if (mode_ != Mode::Caps) mode_ = Mode::Auto; pending_tap_ = false; }
    void edited() { inserted(); }
    void otherAction() { pending_tap_ = false; }
    static bool sentenceStart(const std::string& text);
private:
    enum class Mode { Auto, Lower, Once, Caps };
    Mode mode_ = Mode::Auto;
    bool automatic_ = false, pending_tap_ = false;
    uint64_t last_tap_ = 0;
};
