#include "keyboard.h"
#include <algorithm>
#include <iterator>

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

void Keyboard::open(const std::string& title, bool secret, const std::string& initial) {
    submitted_ = cancelled_ = false; text_ = initial;
    std::fill(std::begin(title_), std::end(title_), 0); std::fill(std::begin(initial_), std::end(initial_), 0); std::fill(std::begin(input_), std::end(input_), 0);
    utf8_to_utf16(title, title_, std::size(title_)); utf8_to_utf16(initial, initial_, std::size(initial_));
    sceImeDialogParamInit(&param_);
    param_.supportedLanguages = 0x0001ffff; param_.languagesForced = SCE_FALSE;
    // VitaSDK doesn't expose a password IME type on all SDK revisions.
    // Authentication still stays inside the system dialog and is sent directly to TDLib.
    (void)secret;
    param_.type = SCE_IME_TYPE_DEFAULT;
    param_.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    param_.title = title_; param_.maxTextLength = MaxTextLength;
    param_.initialText = initial_; param_.inputTextBuffer = input_;
    active_ = sceImeDialogInit(&param_) >= 0;
    if (!active_) cancelled_ = true;
}

void Keyboard::close() { if (active_) sceImeDialogTerm(); active_ = submitted_ = cancelled_ = false; }

void Keyboard::handle(unsigned int) {
    if (!active_ || sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return;
    SceImeDialogResult result{}; sceImeDialogGetResult(&result);
    if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) { text_ = utf16_to_utf8(input_); submitted_ = true; }
    else cancelled_ = true;
    sceImeDialogTerm(); active_ = false;
}

void Keyboard::draw(vita2d_pgf*) const {}
