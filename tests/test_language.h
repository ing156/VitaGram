#pragma once
#include "i18n.h"

#ifndef VITAGRAM_LANG_DIR
#define VITAGRAM_LANG_DIR "assets/lang"
#endif

// Tests that check Russian texts select the language explicitly instead of relying on the system.
inline void use_test_language(const char* code = "ru") { i18n::init(VITAGRAM_LANG_DIR, code); }
