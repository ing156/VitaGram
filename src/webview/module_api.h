#pragma once
#include <stddef.h>
#include <stdint.h>

// Only C values cross the module boundary; each side owns its allocations.
#define VITAGRAM_WEBKIT_ABI 1u
#define VITAGRAM_WEBKIT_ID "webkit-abi1-r1"

struct vita2d_texture;
struct WebKitHost {
    uint32_t abi, size;
    struct vita2d_texture* (*texture_create)(unsigned, unsigned, unsigned);
    void (*texture_free)(struct vita2d_texture*);
    void* (*texture_data)(const struct vita2d_texture*);
    unsigned (*texture_stride)(const struct vita2d_texture*);
    void (*diagnostic)(unsigned, int64_t);
};
struct WebKitApi {
    uint32_t abi, size;
    int (*open)(const char*, int, int, const char*, int, const char*, const char*, int);
    void (*close)(void);
    void (*shutdown)(void);
    int (*active)(void);
    void (*pump)(unsigned, int);
    void (*move)(int, int);
    void (*click)(int, int, int);
    void (*scroll)(int, int);
    void (*orientation)(int, int);
    int (*rotation)(void);
    int (*fullscreen)(void);
    void (*font_scale)(float);
    void (*evaluate)(const char*);
    void (*event)(const char*, const char*);
    const char* (*take_events)(void);
    const char* (*take_link)(void);
    const char* (*error)(void);
    int (*loading)(void);
    struct vita2d_texture* (*texture)(void);
};
struct WebKitStart {
    uint32_t abi, size;
    struct WebKitHost host;
    struct WebKitApi api;
};

#ifdef __cplusplus
inline bool webkit_api_valid(const WebKitApi& api) {
    return api.abi == VITAGRAM_WEBKIT_ABI && api.size == sizeof(WebKitApi) &&
        api.open && api.close && api.shutdown && api.active && api.pump && api.move && api.click &&
        api.scroll && api.orientation && api.rotation && api.fullscreen && api.font_scale &&
        api.evaluate && api.event && api.take_events && api.take_link && api.error && api.loading && api.texture;
}
#endif
