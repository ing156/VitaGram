#include "keyboard_case.h"
#include <cassert>
#include <cstdio>

int main() {
    for (const char* text : {"", "   ", "\"", "«", "¿", "hello. ", "Привет. ", "Done! ", "Really? ", "Wait… ",
        "Done?!   ", "«Привет.» ", "Done. “", "Done.\" ", "hello\n", "Dr.\n", "т.д.\n  ",
        "hello.\xc2\xa0", "Done。", "Done！", "Done？"}) assert(KeyboardCase::sentenceStart(text));
    for (const char* text : {"hello", "hello.", "hello, ", "part: ", "3.14 ", "test@example.com ",
        "https://example.com ", "photo.jpg ", "Dr. ", "PROF. ", "Mr. ", "т.е. ", "Т.Д. ", "ул. ",
        "г. ", "e.g. ", "i.e. ", "z.B. ", "a ‘", "a."}) assert(!KeyboardCase::sentenceStart(text));
    KeyboardCase state;
    state.reset(false); assert(!state.upper(""));
    state.shift(0, "word "); assert(state.upper("word ") && !state.caps());
    state.inserted(); assert(!state.upper("word A"));
    state.shift(1000, "word "); state.shift(1000 + KeyboardCase::doubleTapDelay, "word ");
    assert(state.caps()); state.inserted(); state.edited(); assert(state.upper("x"));
    state.shift(999999, ""); assert(!state.upper("") && !state.caps());
    state.reset(false); state.shift(1000, ""); state.shift(1001+KeyboardCase::doubleTapDelay, "");
    assert(!state.caps() && !state.upper(""));
    state.reset(true); assert(state.upper(""));
    state.shift(100, ""); assert(!state.upper("")); // Explicitly override automatic uppercase.
    state.shift(200, ""); assert(state.caps());
    state.shift(300, ""); assert(!state.upper(""));
    state.inserted(); assert(!state.upper("a"));
    state.edited(); assert(state.upper(""));
    state.shift(500, "word "); state.otherAction(); state.shift(600, "word "); assert(!state.caps());
    state.reset(true); state.shift(100, "word "); state.inserted(); state.shift(200, "word A");
    assert(state.upper("word A") && !state.caps()); // A character breaks a double tap.
    std::puts("keyboard case: sentence context, abbreviations, Unicode punctuation, one-shot/locked Shift and double-tap boundaries: PASS");
}
