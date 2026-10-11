#pragma once
// Checks a TGS sticker's Lottie JSON before rlottie sees it. rlottie parses the whole model and
// decodes embedded raster images with its own loader, without the budgets of the app's image
// decoder, so anything Telegram stickers never contain is refused here: image layers and assets,
// expressions, oversized canvases or frame counts, very deep nesting, very many nodes or layers.
// Telegram's own TGS rules (512x512, up to 60 fps, 3 s, 64 KiB, no images or expressions) are well
// inside these limits, which leave room for animated emoji and reactions.
#include <jansson.h>
#include <cmath>
#include <cstring>
#include <string>

namespace tgs_policy {
constexpr size_t kMaxPacked = 256 * 1024;  // gzip file
constexpr size_t kMaxJson = 2 * 1024 * 1024;
constexpr int kMaxSide = 1024, kMaxDepth = 128;
constexpr double kMaxFps = 120, kMaxFrames = 1800;
constexpr size_t kMaxNodes = 200000, kMaxLayers = 2000;

namespace detail {
struct Walk { size_t nodes = 0, layers = 0; const char* why = nullptr; };

inline bool number(json_t* v, double& out) {
    if (!json_is_number(v)) return false;
    out = json_number_value(v);
    return std::isfinite(out);
}

inline bool walk(json_t* node, int depth, Walk& w) {
    if (++w.nodes > kMaxNodes) { w.why = "too many JSON nodes"; return false; }
    if (depth > kMaxDepth) { w.why = "nested too deeply"; return false; }
    if (json_is_array(node)) {
        size_t i; json_t* item;
        json_array_foreach(node, i, item) if (!walk(item, depth + 1, w)) return false;
        return true;
    }
    if (!json_is_object(node)) return true;
    // A layer: has "ty" and "ip"/"op". Type 2 is an image layer.
    json_t* ty = json_object_get(node, "ty");
    if (json_is_integer(ty) && (json_object_get(node, "ip") || json_object_get(node, "op"))) {
        if (++w.layers > kMaxLayers) { w.why = "too many layers"; return false; }
        if (json_integer_value(ty) == 2) { w.why = "image layer"; return false; }
    }
    // A string "p" is an image source anywhere it appears (shape positions are objects).
    if (json_is_string(json_object_get(node, "p"))) { w.why = "embedded image"; return false; }
    // Expressions are code strings in "x" on animated properties.
    if (json_is_string(json_object_get(node, "x"))) { w.why = "expression"; return false; }
    const char* key; json_t* value;
    json_object_foreach(node, key, value) {
        // rlottie's parseAsset turns an asset into an image as soon as it meets "p", "u" or "e", in any
        // order and next to "layers" or without "id": an asset may carry none of them.
        if (!std::strcmp(key, "assets") && json_is_array(value)) {
            size_t i; json_t* asset;
            json_array_foreach(value, i, asset)
                if (json_is_object(asset) && (json_object_get(asset, "p") || json_object_get(asset, "u") || json_object_get(asset, "e"))) {
                    w.why = "embedded image"; return false;
                }
        }
        if (!walk(value, depth + 1, w)) return false;
    }
    return true;
}
}  // namespace detail

// True when `json` is a Lottie document VitaGram is willing to hand to rlottie. `why` (optional)
// receives a short English reason otherwise.
inline bool acceptable(const std::string& json, std::string* why = nullptr) {
    auto refuse = [&](const char* reason) { if (why) *why = reason; return false; };
    if (json.empty() || json.size() > kMaxJson) return refuse("size");
    json_error_t error{};
    // Duplicate keys are refused: jansson keeps the last value, rlottie's parser sees every one, and
    // the two must not disagree about what the document contains.
    json_t* root = json_loadb(json.data(), json.size(), JSON_REJECT_DUPLICATES, &error);
    if (!json_is_object(root)) { if (root) json_decref(root); return refuse("not a JSON object or duplicate keys"); }
    double w = 0, h = 0, fr = 0, ip = 0, op = 0;
    const bool header = detail::number(json_object_get(root, "w"), w) && detail::number(json_object_get(root, "h"), h) &&
                        detail::number(json_object_get(root, "fr"), fr) && detail::number(json_object_get(root, "ip"), ip) &&
                        detail::number(json_object_get(root, "op"), op);
    const char* reason = nullptr;
    if (!header) reason = "missing w/h/fr/ip/op";
    else if (w < 1 || h < 1 || w > kMaxSide || h > kMaxSide) reason = "canvas size";
    else if (fr <= 0 || fr > kMaxFps) reason = "frame rate";
    else if (ip < 0 || op <= ip || op - ip > kMaxFrames) reason = "frame range";
    else {
        detail::Walk walk;
        if (!detail::walk(root, 0, walk)) reason = walk.why;
    }
    json_decref(root);
    return reason ? refuse(reason) : true;
}
}  // namespace tgs_policy
