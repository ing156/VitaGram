#pragma once
#include <jansson.h>
#include <cstdlib>
#include <string>

inline std::string webapp_cancelled_invoice(json_t* data) {
    auto* slug = json_object_get(data, "slug");
    if (!json_is_string(slug) || !json_string_length(slug) || json_string_length(slug) > 8192) return {};
    const std::string name(json_string_value(slug), json_string_length(slug));
    if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_=") != std::string::npos) return {};
    // No payment was attempted: close the SDK's pending invoice as cancelled.
    auto* response = json_pack("{s:O,s:s}", "slug", slug, "status", "cancelled");
    auto* encoded = response ? json_dumps(response, JSON_COMPACT) : nullptr;
    std::string result = encoded ? encoded : "";
    std::free(encoded); json_decref(response);
    return result;
}
