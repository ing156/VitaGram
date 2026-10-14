#pragma once
#include <jansson.h>
#include <cstring>
#include <string>

namespace user_identity {
inline std::string name(json_t* user) {
    auto field = [&](const char* key) {
        const char* value = json_string_value(json_object_get(user, key));
        std::string text = value ? value : "";
        const auto begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) return std::string{};
        return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
    };
    std::string first = field("first_name"), last = field("last_name");
    return first.empty() ? last : last.empty() ? first : first + " " + last;
}
inline bool deleted(json_t* user) {
    const char* type = json_string_value(json_object_get(json_object_get(user, "type"), "@type"));
    // Old UI caches have no user type. Preserve their empty-name fallback.
    return type ? !std::strcmp(type, "userTypeDeleted") : name(user).empty();
}
}
