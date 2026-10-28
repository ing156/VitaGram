#pragma once
#include <hb.h>
#include <unicode/uscript.h>
static inline hb_script_t hb_icu_script_to_script(UScriptCode script) {
    const char* name = uscript_getShortName(script);
    return name ? hb_script_from_string(name, -1) : HB_SCRIPT_INVALID;
}
