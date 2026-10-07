#pragma once

#include <jansson.h>
#include <string>
#include <cstring>

namespace rich_message {
inline const char* field(json_t* object, const char* key) {
    auto* value = json_object_get(object, key);
    return json_is_string(value) ? json_string_value(value) : "";
}

inline std::string text(json_t* node, int depth = 0) {
    if (!json_is_object(node) || depth > 24) return {};
    const char* type = field(node, "@type");
    if (!std::strcmp(type, "richTextPlain")) return field(node, "text");
    if (!std::strcmp(type, "richTextCustomEmoji")) return field(node, "alternative_text");
    if (!std::strcmp(type, "richTextMathematicalExpression")) return field(node, "expression");
    if (!std::strcmp(type, "richTexts")) {
        std::string result;
        auto* parts = json_object_get(node, "texts");
        for (size_t i = 0; i < json_array_size(parts) && i < 1024; ++i)
            result += text(json_array_get(parts, i), depth + 1);
        return result;
    }
    // Formatting and links wrap another RichText object.
    return text(json_object_get(node, "text"), depth + 1);
}

inline void append(std::string& result, const std::string& part) {
    if (part.empty()) return;
    if (!result.empty()) result += '\n';
    result += part;
}

inline void blocks(std::string& result, json_t* items, int depth = 0) {
    if (!json_is_array(items) || depth > 16) return;
    for (size_t i = 0; i < json_array_size(items) && i < 512; ++i) {
        auto* block = json_array_get(items, i);
        if (!json_is_object(block)) continue;
        const char* type = field(block, "@type");
        const char* key = nullptr;
        if (!std::strcmp(type, "pageBlockTitle")) key = "title";
        else if (!std::strcmp(type, "pageBlockSubtitle")) key = "subtitle";
        else if (!std::strcmp(type, "pageBlockHeader")) key = "header";
        else if (!std::strcmp(type, "pageBlockSubheader")) key = "subheader";
        else if (!std::strcmp(type, "pageBlockKicker")) key = "kicker";
        else if (!std::strcmp(type, "pageBlockFooter")) key = "footer";
        else if (!std::strcmp(type, "pageBlockAuthorDate")) key = "author";
        else if (!std::strcmp(type, "pageBlockSectionHeading") || !std::strcmp(type, "pageBlockParagraph") ||
                 !std::strcmp(type, "pageBlockPreformatted") || !std::strcmp(type, "pageBlockThinking") ||
                 !std::strcmp(type, "pageBlockExpandableBlockQuote") || !std::strcmp(type, "pageBlockPullQuote")) key = "text";
        if (key) append(result, text(json_object_get(block, key)));
        if (!std::strcmp(type, "pageBlockMathematicalExpression")) append(result, field(block, "expression"));
        auto* caption = json_object_get(block, "caption");
        if (json_is_object(caption) && std::strcmp(type, "pageBlockTable"))
            append(result, text(json_object_get(caption, "text")));
        if (!std::strcmp(type, "pageBlockTable")) {
            append(result, text(caption));
            auto* rows = json_object_get(block, "cells");
            for (size_t row = 0; row < json_array_size(rows) && row < 128; ++row) {
                auto* cells = json_array_get(rows, row);
                std::string line;
                for (size_t col = 0; col < json_array_size(cells) && col < 32; ++col) {
                    auto cell = text(json_object_get(json_array_get(cells, col), "text"));
                    if (!cell.empty()) { if (!line.empty()) line += " | "; line += cell; }
                }
                append(result, line);
            }
        }
        if (!std::strcmp(type, "pageBlockList")) {
            auto* entries = json_object_get(block, "items");
            for (size_t j = 0; j < json_array_size(entries) && j < 128; ++j) {
                auto* entry = json_array_get(entries, j);
                std::string nested;
                blocks(nested, json_object_get(entry, "blocks"), depth + 1);
                if (!nested.empty()) append(result, std::string(field(entry, "label")) + nested);
            }
        }
        blocks(result, json_object_get(block, "blocks"), depth + 1);
        if (!std::strcmp(type, "pageBlockCover")) {
            auto* cover = json_object_get(block, "cover");
            auto* wrapper = json_array();
            if (cover) json_array_append(wrapper, cover);
            blocks(result, wrapper, depth + 1);
            json_decref(wrapper);
        }
    }
}

template <typename Function>
inline void visit_blocks(json_t* items, Function& visit, int depth = 0) {
    if (!json_is_array(items) || depth > 16) return;
    for (size_t i = 0; i < json_array_size(items) && i < 512; ++i) {
        auto* block = json_array_get(items, i);
        if (!json_is_object(block)) continue;
        visit(block);
        visit_blocks(json_object_get(block, "blocks"), visit, depth + 1);
        auto* list = json_object_get(block, "items");
        for (size_t j = 0; j < json_array_size(list) && j < 128; ++j)
            visit_blocks(json_object_get(json_array_get(list, j), "blocks"), visit, depth + 1);
        auto* cover = json_object_get(block, "cover");
        if (json_is_object(cover)) {
            auto* wrapper = json_array();
            json_array_append(wrapper, cover);
            visit_blocks(wrapper, visit, depth + 1);
            json_decref(wrapper);
        }
    }
}

template <typename Function>
inline void visit_links(json_t* node, Function& visit, int depth = 0) {
    if (depth > 32) return;
    if (json_is_object(node)) {
        const char* type = field(node, "@type");
        if (!std::strcmp(type, "richTextUrl") || !std::strcmp(type, "richTextReferenceLink") ||
            !std::strcmp(type, "pageBlockPhoto") || !std::strcmp(type, "pageBlockEmbedded") ||
            !std::strcmp(type, "pageBlockEmbeddedPost")) visit(field(node, "url"));
        const char* key; json_t* value;
        json_object_foreach(node, key, value) visit_links(value, visit, depth + 1);
    } else if (json_is_array(node)) {
        for (size_t i = 0; i < json_array_size(node) && i < 1024; ++i)
            visit_links(json_array_get(node, i), visit, depth + 1);
    }
}
}
