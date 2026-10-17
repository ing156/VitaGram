#include "keyboard.h"
#include "keyboard_languages.h"
#include "i18n.h"
#include <algorithm>
#include <iterator>
#include <vita2d.h>

namespace {
void utf8_to_utf16(const std::string& s, unsigned short* d, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < s.size() && o + 1 < cap;) {
        unsigned int cp; unsigned char c = static_cast<unsigned char>(s[i++]);
        if (c < 0x80) cp = c;
        else if ((c & 0xe0) == 0xc0 && i < s.size()) cp = ((c & 31) << 6) | (static_cast<unsigned char>(s[i++]) & 63);
        else if ((c & 0xf0) == 0xe0 && i + 1 < s.size()) { cp = ((c & 15) << 12) | ((static_cast<unsigned char>(s[i]) & 63) << 6) | (static_cast<unsigned char>(s[i + 1]) & 63); i += 2; }
        else if ((c & 0xf8) == 0xf0 && i + 2 < s.size()) { cp = ((c & 7) << 18) | ((static_cast<unsigned char>(s[i]) & 63) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 63) << 6) | (static_cast<unsigned char>(s[i + 2]) & 63); i += 3; }
        else cp = 0xfffd;
        if (cp > 0xffff && o + 2 < cap) { cp -= 0x10000; d[o++] = 0xd800 + (cp >> 10); d[o++] = 0xdc00 + (cp & 0x3ff); }
        else d[o++] = static_cast<unsigned short>(cp);
    }
    d[o] = 0;
}

std::string utf16_to_utf8(const unsigned short* s) {
    std::string out;
    for (size_t i = 0; s[i]; ++i) {
        unsigned int cp = s[i];
        if (cp >= 0xd800 && cp <= 0xdbff && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff) cp = 0x10000 + ((cp - 0xd800) << 10) + (s[++i] - 0xdc00);
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xc0 | cp >> 6); out += static_cast<char>(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xe0 | cp >> 12); out += static_cast<char>(0x80 | (cp >> 6 & 63)); out += static_cast<char>(0x80 | (cp & 63)); }
        else { out += static_cast<char>(0xf0 | cp >> 18); out += static_cast<char>(0x80 | (cp >> 12 & 63)); out += static_cast<char>(0x80 | (cp >> 6 & 63)); out += static_cast<char>(0x80 | (cp & 63)); }
    }
    return out;
}
}

Keyboard::Keyboard() = default;
void Keyboard::configure(bool custom, bool dark, const std::vector<std::string>& languages) {
    prefer_custom_ = custom; dark_ = dark;
    languages_ = keyboard_languages::normalize(languages);
}

void Keyboard::open(const std::string& title, bool secret, const std::string& initial, bool message) {
    if (active_) return;
    caption_ = title; secret_ = secret; message_ = message;
    custom_active_ = prefer_custom_;
    if (custom_active_) {
        clear();
        custom_.setLanguages(languages_);
        custom_.open(initial, secret, message, MaxTextLength, true);
        submitted_ = cancelled_ = false;
        suppress_opening_input_ = true;
        active_ = true;
        if (!custom_.systemRequested()) return;
        active_ = custom_active_ = false;
        openSystem(title, secret, initial, custom_.language());
        custom_.clear();
        return;
    }
    openSystem(title, secret, initial, "");
}

void Keyboard::openSystem(const std::string& title, bool secret, const std::string& initial, const std::string& language) {
    // IME owns GPU resources too. Drain both rendering and presentation before
    // creating a dialog or reusing the buffers from the previous dialog.
    vita2d_wait_rendering_done();
    sceGxmDisplayQueueFinish();
    submitted_ = cancelled_ = false; text_ = initial;
    std::fill(std::begin(title_), std::end(title_), 0); std::fill(std::begin(initial_), std::end(initial_), 0); std::fill(std::begin(input_), std::end(input_), 0);
    utf8_to_utf16(title, title_, std::size(title_)); utf8_to_utf16(initial, initial_, std::size(initial_));
    sceImeDialogParamInit(&param_);
    static const uint64_t masks[] = {
        SCE_IME_LANGUAGE_ENGLISH | SCE_IME_LANGUAGE_ENGLISH_GB, SCE_IME_LANGUAGE_RUSSIAN,
        SCE_IME_LANGUAGE_GERMAN, SCE_IME_LANGUAGE_FRENCH, SCE_IME_LANGUAGE_SPANISH,
        SCE_IME_LANGUAGE_ITALIAN, SCE_IME_LANGUAGE_DUTCH, SCE_IME_LANGUAGE_PORTUGUESE,
        SCE_IME_LANGUAGE_PORTUGUESE_BR, SCE_IME_LANGUAGE_SWEDISH, SCE_IME_LANGUAGE_DANISH,
        SCE_IME_LANGUAGE_NORWEGIAN, SCE_IME_LANGUAGE_FINNISH, SCE_IME_LANGUAGE_POLISH,
        SCE_IME_LANGUAGE_TURKISH, SCE_IME_LANGUAGE_JAPANESE, SCE_IME_LANGUAGE_KOREAN,
        SCE_IME_LANGUAGE_SIMPLIFIED_CHINESE, SCE_IME_LANGUAGE_TRADITIONAL_CHINESE
    };
    const auto selected = language.empty() ? keyboard_languages::resolve(languages_, i18n::systemLanguage()) :
        std::vector<std::string>{language};
    param_.supportedLanguages = 0;
    for (size_t i = 0; i < keyboard_languages::supported().size(); ++i)
        if (keyboard_languages::contains(selected, keyboard_languages::supported()[i])) param_.supportedLanguages |= masks[i];
    param_.languagesForced = SCE_TRUE;
    // Secrets (2FA password, vault passphrase) are masked by the system dialog.
    param_.type = SCE_IME_TYPE_DEFAULT;
    param_.textBoxMode = secret ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    // No predictive text or suggestions for secrets, so the system keyboard has nothing to learn from
    // a passphrase; automatic capitalization would also change what the user typed.
    param_.option = secret ? (SCE_IME_OPTION_NO_ASSISTANCE | SCE_IME_OPTION_NO_AUTO_CAPITALIZATION) : 0;
    param_.title = title_; param_.maxTextLength = MaxTextLength;
    param_.initialText = initial_; param_.inputTextBuffer = input_;
    active_ = sceImeDialogInit(&param_) >= 0;
    if (!active_) cancelled_ = true;
}

void Keyboard::close() {
    if (systemActive()) {
        vita2d_wait_rendering_done();
        sceGxmDisplayQueueFinish();
        sceImeDialogTerm();
    }
    active_ = submitted_ = cancelled_ = custom_active_ = false;
    clear();
}

void Keyboard::clear() {
    custom_.clear();
    std::fill(text_.begin(), text_.end(), '\0');
    text_.clear();
    std::fill(std::begin(input_), std::end(input_), 0);
    std::fill(std::begin(initial_), std::end(initial_), 0);
}

void Keyboard::handle(unsigned int pressed, unsigned int held, bool touching, int x, int y, uint64_t now) {
    if (active_ && custom_active_) {
        // The button/tap that opened the dialog belongs to the underlying screen.
        if (suppress_opening_input_) {
            if (!held && !touching) suppress_opening_input_ = false;
            return;
        }
        const auto result = custom_.handle(pressed, held, touching, x, y, now);
        if (result == CustomKeyboard::Result::System) {
            const std::string initial = custom_.text();
            active_ = custom_active_ = false;
            openSystem(caption_, secret_, initial, custom_.language());
            custom_.clear();
            return;
        }
        if (result == CustomKeyboard::Result::Submitted) { text_ = custom_.text(); submitted_ = true; }
        if (result == CustomKeyboard::Result::Cancelled) cancelled_ = true;
        if (submitted_ || cancelled_) active_ = false;
        return;
    }
    if (!active_ || sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return;
    SceImeDialogResult result{}; sceImeDialogGetResult(&result);
    if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) { text_ = utf16_to_utf8(input_); submitted_ = true; }
    else cancelled_ = true;
    // A finished dialog may still have a submitted frame in the display queue.
    vita2d_wait_rendering_done();
    sceGxmDisplayQueueFinish();
    sceImeDialogTerm(); active_ = false;
}

void Keyboard::draw(vita2d_pgf* font) const {
    if (active_ && custom_active_) custom_.draw(font, dark_, caption_);
}
