#pragma once
#include <jansson.h>
#include <cstring>
#include <initializer_list>

inline json_t* sticker_preview_file(json_t* file) {
    json_t* local = json_object_get(file, "local");
    const char* path = json_string_value(json_object_get(local, "path"));
    return json_integer_value(json_object_get(file, "id")) > 0 ||
        (json_is_true(json_object_get(local, "is_downloading_completed")) && path && *path) ? file : nullptr;
}
// Return a borrowed static image file, never a TGS/WebM animation.
inline json_t* sticker_static_thumbnail(json_t* thumbnail) {
    const char* type = json_string_value(json_object_get(json_object_get(thumbnail, "format"), "@type"));
    if (!type || (std::strcmp(type, "thumbnailFormatWebp") &&
        std::strcmp(type, "thumbnailFormatPng") && std::strcmp(type, "thumbnailFormatJpeg"))) return nullptr;
    return sticker_preview_file(json_object_get(thumbnail, "file"));
}
inline json_t* sticker_pack_preview(json_t* set) {
    if (auto* file = sticker_static_thumbnail(json_object_get(set, "thumbnail"))) return file;
    for (const char* field : {"covers", "stickers"}) {
        size_t i; json_t* sticker;
        json_array_foreach(json_object_get(set, field), i, sticker) {
            if (auto* file = sticker_static_thumbnail(json_object_get(sticker, "thumbnail"))) return file;
            const char* format = json_string_value(json_object_get(json_object_get(sticker, "format"), "@type"));
            if (format && !std::strcmp(format, "stickerFormatWebp"))
                if (auto* file = sticker_preview_file(json_object_get(sticker, "sticker"))) return file;
        }
    }
    return nullptr;
}
