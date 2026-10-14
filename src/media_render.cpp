#include "performance.h"
#include "utf8_decode.h"
#include "media_render.h"
#include "vault.h"
#include "font_fallback.h"
#include "text_layout_cache.h"
#include "text_font.h"
#include "ui_skin.h"
#include <memory>
#include <array>
#include <atomic>
#include <pthread.h>
#include "cpu_scheduler.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <webp/decode.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
// Base text size in pixels at 100%; each UI element applies its own scale.
constexpr int kBaseFontSizePx = 30;
FT_Library library = nullptr;
FT_Face face = nullptr;
// Faces retain their in-memory font buffers until the raster cache is released.
struct TextFace { FT_Face coverage=nullptr; std::unique_ptr<TextFont> font; std::vector<unsigned char> data; };
std::array<TextFace,6> text_faces;
// Slot 2 holds the font of the interface language (Korean/Chinese) and is placed ahead of the
// Japanese font so shared Han characters use the glyph shapes of that language. It has no fixed
// path: rich_text_set_language_font() fills it.
constexpr size_t language_slot = 2;
const char* const text_paths[] = {"app0:assets/NotoSans-Regular.ttf",
    "app0:assets/NotoSansCoptic-Regular.ttf", nullptr, "app0:assets/NotoSansJP-Regular.otf", "app0:assets/NotoSansMath-Regular.ttf", "app0:assets/NotoSansSymbols2-Regular.ttf"};
std::string language_font_path;
// One active CJK face and at most one recently used/prepared face.
TextFace spare_language_face;
std::string spare_language_path;
struct LanguageLoader {
    pthread_t thread{};
    bool started = false;
    std::atomic<bool> done{false};
    std::string reading, wanted, failed;
    std::vector<unsigned char> data;
} language_loader;
const char* const emoji_path = "app0:assets/NotoColorEmoji.ttf";
// Fallback fonts and the emoji font (~11 MB) are read off the UI thread after
// the main font; only the UI thread creates FreeType faces and vita2d fonts.
struct FontLoader {
    pthread_t thread{}; bool started=false;
    std::array<std::vector<unsigned char>,6> text;
    std::vector<unsigned char> emoji;
    std::atomic<bool> done{false}; std::atomic<bool> stop{false};
    bool adopted=false; uint64_t started_at=0;
} loader;
std::vector<unsigned char> emoji_data; // Owns the memory face's bytes.
unsigned font_revision = 0;
TextWidthCache widths;
TextWidthCache run_widths;
struct FontChoice { uint32_t code=0;size_t index=0;bool valid=false; };
std::array<FontChoice,4096> font_choices;

TextFont* font_for(uint32_t code) {
    auto& choice=font_choices[code%font_choices.size()];
    if (choice.valid && choice.code==code) return text_faces[choice.index].font.get();
    size_t index=font_fallback_index(code,text_faces.size(),[](size_t i,uint32_t c) {
        return text_faces[i].font && text_faces[i].coverage && FT_Get_Char_Index(text_faces[i].coverage,c);
    });
    choice={code,index,true};
    return text_faces[index].font.get();
}
struct Glyph { uint32_t code; vita2d_texture* texture; uint64_t used; };
std::vector<Glyph> cache;
std::vector<uint32_t> requested;
std::array<vita2d_texture*,9> controller_icons{};
uint64_t clock_value = 0;

uint32_t next_code(const char*& p) {
    uint32_t code;
    p += decode_utf8_scalar(p, code);
    return code;
}

bool is_emoji(uint32_t c) {
    return face && ((c >= 0x1f000 && c <= 0x1faff) || (c >= 0x2600 && c <= 0x27bf) ||
        (c >= 0x2300 && c <= 0x23ff) || (c >= 0x2b00 && c <= 0x2bff)) && FT_Get_Char_Index(face, c);
}
vita2d_texture* glyph_texture(uint32_t code) {
    for (auto& item : cache) if (item.code == code) { item.used = ++clock_value; return item.texture; }
    if (requested.size() < 128 && std::find(requested.begin(), requested.end(), code) == requested.end())
        requested.push_back(code);
    return nullptr;
}
int render(vita2d_pgf* font, int x, int baseline, unsigned int color, float scale, const char* p, bool draw) {
    float advance = 0;
    const unsigned int text_size = std::max(1, (int)(kBaseFontSizePx * scale + 0.5f));
    std::string run;
    TextFont* text_font = text_faces[0].font.get();
    auto flush = [&]() {
        if (run.empty()) return;
        if (text_font) {
            if (draw) text_font->render(x + static_cast<int>(advance), baseline, color, text_size, run.c_str(), true);
            advance += run_widths.get(run.c_str(),reinterpret_cast<uintptr_t>(text_font),float(text_size),[&]{return text_font->render(0,0,0,text_size,run.c_str(),false);});
        } else if (font) {
            // Emergency fallback only; normal UI always uses the packaged Noto font.
            if (draw) vita2d_pgf_draw_text(font, x + static_cast<int>(advance), baseline, color, scale, run.c_str());
            advance += run_widths.get(run.c_str(),reinterpret_cast<uintptr_t>(font),scale,[&]{return vita2d_pgf_text_width(font,scale,run.c_str());});
        }
        run.clear();
    };
    while (*p) {
        const char* start = p;
        uint32_t code = next_code(p);
        // Basic Unicode glyphs only: complex ZWJ sequences are displayed as components.
        if (code == 0xfe0f || code == 0xfe0e || code == 0x200d) continue;
        // Private-use code points are reserved for Vita controller buttons.
        if (code >= 0xe000 && code < 0xe000 + controller_icons.size()) {
            flush();
            // All controller icons share one on-screen height in the 960x544 UI.
            constexpr float size = 22.0f;
            ui_skin::Sprite sprite;
            if (ui_skin::button(code - 0xe000, sprite)) {
                const float factor = size / sprite.vh, width = sprite.vw * factor;
                if (draw) {
                    const float top = baseline - size + (size - text_size * 0.7f) * 0.5f + 1.5f;
                    vita2d_draw_texture_part_scale(sprite.texture, x + advance - sprite.vx * factor, top - sprite.vy * factor,
                        sprite.x, sprite.y, sprite.w, sprite.h, factor, factor);
                }
                advance += width + 3 * scale;
                continue;
            }
            auto* icon = controller_icons[code - 0xe000];
            float width = icon ? size * vita2d_texture_get_width(icon) / vita2d_texture_get_height(icon)
                               : size * (code >= 0xe005 ? 56.0f / 24.0f : 1.0f);
            if (draw && icon) {
                // Center the icon on the visible text body instead of its baseline.
                float top = baseline - size + (size - text_size * 0.7f) * 0.5f + 1.5f;
                vita2d_draw_texture_scale(icon, x + advance, top,
                    width / vita2d_texture_get_width(icon), size / vita2d_texture_get_height(icon));
            }
            advance += width + 3 * scale;
            continue;
        }
        if (!is_emoji(code)) {
            TextFont* selected = font_for(code);
            if (selected != text_font) { flush(); text_font = selected; }
            run.append(start, p - start); continue;
        }
        flush();
        float size = text_size;
        if (draw) {
            vita2d_texture* texture = glyph_texture(code);
            if (texture) vita2d_draw_texture_scale(texture, x + advance, baseline - size + 3 * scale,
                size / vita2d_texture_get_width(texture), size / vita2d_texture_get_height(texture));
            else vita2d_draw_rectangle(x + advance + 3, baseline - size + 5, size - 6, 2, color);
        }
        advance += size + 2 * scale;
    }
    flush();
    return static_cast<int>(advance + 0.5f);
}
}

namespace {
bool read_file(const char* path, std::vector<unsigned char>& out) {
    FILE* file=std::fopen(path,"rb");
    if (!file) return false;
    bool ok=!std::fseek(file,0,SEEK_END);
    const long size=ok ? std::ftell(file) : -1;
    ok=ok && size>0 && !std::fseek(file,0,SEEK_SET);
    if (ok) { out.resize(static_cast<size_t>(size)); ok=std::fread(out.data(),1,out.size(),file)==out.size(); }
    std::fclose(file);
    if (!ok) std::vector<unsigned char>().swap(out);
    return ok;
}

void* load_fallback_fonts(void*) {
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Storage);
    for (size_t i=1; i<loader.text.size() && !loader.stop; ++i) if (text_paths[i]) read_file(text_paths[i],loader.text[i]);
    if (!loader.stop) read_file(emoji_path,loader.emoji);
    loader.done=true;
    return nullptr;
}
bool create_text_face(TextFace& text, std::vector<unsigned char>&& data) {
    text.data=std::move(data);
    if (text.data.empty() ||
        FT_New_Memory_Face(library,text.data.data(),static_cast<FT_Long>(text.data.size()),0,&text.coverage)) {
        text.coverage=nullptr; std::vector<unsigned char>().swap(text.data); return false;
    }
    text.font = std::make_unique<TextFont>(text.coverage);
    return text.font != nullptr;
}
bool adopt_text_face(size_t index, std::vector<unsigned char>&& data) {
    return create_text_face(text_faces[index], std::move(data));
}
void release_text_face(TextFace& text) {
    text.font.reset();
    if (text.coverage) FT_Done_Face(text.coverage);
    text = {};
}
void* load_language_font(void*) {
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Storage);
    read_file(language_loader.reading.c_str(), language_loader.data);
    language_loader.done = true;
    return nullptr;
}
void poll_language_font() {
    auto& task = language_loader;
    if (!task.started || !task.done) return;
    pthread_join(task.thread, nullptr);
    task.started = false;
    if (task.reading == task.wanted) {
        TextFace candidate;
        if (create_text_face(candidate, std::move(task.data))) {
            vita2d_wait_rendering_done();
            release_text_face(spare_language_face);
            spare_language_face = std::move(candidate);
            spare_language_path = task.reading;
        } else task.failed = task.reading;
    }
    std::vector<unsigned char>().swap(task.data);
}
}

bool rich_text_init() {
    widths.clear();run_widths.clear();font_choices={};
    const char* icon_names[] = {"triangle", "square", "cross", "circle", "dpad", "start", "select", "L", "R"};
    for (size_t i=0; i<controller_icons.size(); ++i) {
        std::string path = std::string(i < 7 ? "app0:assets/controller-icons/" : "app0:ui/themes/dark/buttons/") + icon_names[i] + ".png";
        controller_icons[i] = vita2d_load_PNG_file(path.c_str());
    }
    if (FT_Init_FreeType(&library)) return false;
    // The main font is needed for the first frame; fallbacks arrive later.
    std::vector<unsigned char> main_font;
    if (read_file(text_paths[0],main_font)) adopt_text_face(0,std::move(main_font));
    if (FT_New_Face(library, emoji_path, 0, &face)) face = nullptr;
    if (face && (!face->num_fixed_sizes || FT_Select_Size(face, 0))) { FT_Done_Face(face); face = nullptr; }
    loader.done=false; loader.stop=false; loader.adopted=false; loader.started_at=performance::now();
    pthread_attr_t attributes; pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 64 * 1024);
    loader.started = pthread_create(&loader.thread, &attributes, load_fallback_fonts, nullptr) == 0;
    pthread_attr_destroy(&attributes);
    if (!loader.started) load_fallback_fonts(nullptr); // Synchronous fallback.
    if (!language_font_path.empty()) {
        std::vector<unsigned char> data;
        if (read_file(language_font_path.c_str(), data)) adopt_text_face(language_slot, std::move(data));
    }
    // Keep the text font usable even when the emoji asset cannot be opened.
    return text_faces[0].font && face;
}
void rich_text_skin_changed() {
    const bool skinned = ui_skin::has_buttons();
    const char* icon_names[] = {"triangle", "square", "cross", "circle", "dpad", "start", "select", "L", "R"};
    vita2d_wait_rendering_done();
    for (size_t i=0; i<controller_icons.size(); ++i) {
        if (skinned) { if (controller_icons[i]) { vita2d_free_texture(controller_icons[i]); controller_icons[i]=nullptr; } }
        else if (!controller_icons[i]) {
            const std::string path = std::string(i < 7 ? "app0:assets/controller-icons/" : "app0:ui/themes/dark/buttons/") + icon_names[i] + ".png";
            controller_icons[i] = vita2d_load_PNG_file(path.c_str());
        }
    }
    widths.clear(); run_widths.clear(); ++font_revision;
}
void rich_text_poll() {
    poll_language_font();
    bool reset = false;
    for (const auto& text : text_faces) if (text.font && text.font->needs_reset()) reset = true;
    if (reset) {
        vita2d_wait_rendering_done();
        for (auto& text : text_faces) if (text.font && text.font->needs_reset()) text.font->reset();
        widths.clear(); run_widths.clear();
    }
    if (loader.adopted || !loader.done) return;
    if (loader.started) { pthread_join(loader.thread, nullptr); loader.started=false; }
    loader.adopted=true;
    for (size_t i=1; i<text_faces.size(); ++i) if (text_paths[i]) adopt_text_face(i,std::move(loader.text[i]));
    if (!loader.emoji.empty()) {
        FT_Face memory=nullptr;
        if (!FT_New_Memory_Face(library,loader.emoji.data(),static_cast<FT_Long>(loader.emoji.size()),0,&memory) &&
            memory->num_fixed_sizes && !FT_Select_Size(memory,0)) {
            if (face) FT_Done_Face(face);
            face=memory; emoji_data=std::move(loader.emoji); // Cached emoji textures stay valid.
        } else if (memory) FT_Done_Face(memory);
        std::vector<unsigned char>().swap(loader.emoji);
    }
    // Earlier measurements routed these code points to the main font.
    widths.clear(); run_widths.clear(); font_choices={}; ++font_revision;
    performance::observe(performance::Metric::FontsFallback, performance::now()-loader.started_at);
}
unsigned rich_text_revision() { return font_revision; }
LanguageFontStatus rich_text_japanese_font_status() {
    if (text_faces[3].font) return LanguageFontStatus::Ready;
    return loader.adopted ? LanguageFontStatus::Failed : LanguageFontStatus::Loading;
}
LanguageFontStatus rich_text_prepare_language_font(const char* path) {
    const std::string wanted = path ? path : "";
    auto& task = language_loader;
    if (task.wanted != wanted) { task.wanted = wanted; task.failed.clear(); }
    if (wanted.empty() || (wanted == language_font_path && text_faces[language_slot].font) ||
        (wanted == spare_language_path && spare_language_face.font)) return LanguageFontStatus::Ready;
    if (task.failed == wanted) return LanguageFontStatus::Failed;
    if (!task.started) {
        task.reading = wanted;
        task.done = false;
        pthread_attr_t attributes; pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 64 * 1024);
        task.started = pthread_create(&task.thread, &attributes, load_language_font, nullptr) == 0;
        pthread_attr_destroy(&attributes);
        if (!task.started) { task.failed = wanted; return LanguageFontStatus::Failed; }
    }
    return LanguageFontStatus::Loading;
}
bool rich_text_set_language_font(const char* path) {
    const std::string wanted = path ? path : "";
    if (!library) { language_font_path = wanted; return true; }
    if (wanted == language_font_path && (wanted.empty() || text_faces[language_slot].font)) return true;
    TextFace candidate;
    if (!wanted.empty()) {
        if (wanted == spare_language_path && spare_language_face.font) {
            candidate = std::move(spare_language_face);
            spare_language_face = {};
            spare_language_path.clear();
        } else {
            // Startup callers can load synchronously; settings prepare in the worker first.
            std::vector<unsigned char> data;
            if (!read_file(wanted.c_str(), data) || !create_text_face(candidate, std::move(data))) return false;
        }
    }
    vita2d_wait_rendering_done();
    auto& slot = text_faces[language_slot];
    if (slot.font) {
        release_text_face(spare_language_face);
        spare_language_face = std::move(slot);
        spare_language_path = language_font_path;
    }
    slot = std::move(candidate);
    language_font_path = wanted;
    widths.clear(); run_widths.clear(); font_choices={}; ++font_revision;
    return true;
}
void emoji_load_one() {
    if (!face || requested.empty()) return;
    performance::Scope measurement(performance::Metric::EmojiDecode);
    uint32_t code = requested.front(); requested.erase(requested.begin());
    vita2d_wait_rendering_done();
    vita2d_texture* texture = nullptr;
    if (!FT_Load_Char(face, code, FT_LOAD_COLOR | FT_LOAD_RENDER)) {
        const FT_Bitmap& bitmap = face->glyph->bitmap;
        if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA && bitmap.buffer && bitmap.width && bitmap.rows &&
            bitmap.width <= 256 && bitmap.rows <= 256) {
            texture = vita2d_create_empty_texture(bitmap.width, bitmap.rows);
            if (texture) {
                auto* target = static_cast<unsigned char*>(vita2d_texture_get_datap(texture));
                unsigned int stride = vita2d_texture_get_stride(texture);
                std::memset(target, 0, stride * bitmap.rows);
                for (unsigned int y = 0; y < bitmap.rows; ++y) {
                    const unsigned char* row = bitmap.buffer + (bitmap.pitch >= 0 ? y : bitmap.rows - 1 - y) * std::abs(bitmap.pitch);
                    for (unsigned int x = 0; x < bitmap.width; ++x) {
                        const auto* src = row + x * 4; auto* dst = target + y * stride + x * 4;
                        // FreeType BGRA is premultiplied; vita2d uses straight RGBA.
                        unsigned int a = src[3]; dst[3] = a;
                        for (int c = 0; c < 3; ++c) dst[c] = a ? std::min(255u, (unsigned int)src[2-c] * 255 / a) : 0;
                    }
                }
                vita2d_texture_set_filters(texture, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
            }
        }
    }
    // Each glyph texture occupies a 256 KiB CDRAM block; 64 of them cost 16 MiB.
    if (cache.size() >= 64) {
        auto old = std::min_element(cache.begin(), cache.end(), [](const Glyph& a, const Glyph& b) { return a.used < b.used; });
        if (old->texture) vita2d_free_texture(old->texture);
        *old = {code, texture, ++clock_value};
    } else cache.push_back({code, texture, ++clock_value});
    performance::cdram_glyphs = (int64_t)cache.size() * 256 * 1024;
}
void rich_text_shutdown() {
    if (language_loader.started) {
        pthread_join(language_loader.thread, nullptr);
        language_loader.started = false;
    }
    std::vector<unsigned char>().swap(language_loader.data);
    release_text_face(spare_language_face);
    spare_language_path.clear();
    language_loader.wanted.clear(); language_loader.failed.clear();
    for (auto*& icon : controller_icons) { if (icon) vita2d_free_texture(icon); icon=nullptr; }
    loader.stop=true;
    if (loader.started) { pthread_join(loader.thread, nullptr); loader.started=false; }
    for (auto& data : loader.text) std::vector<unsigned char>().swap(data);
    std::vector<unsigned char>().swap(loader.emoji);
    widths.clear();run_widths.clear();font_choices={};
    for (auto& item : cache) if (item.texture) vita2d_free_texture(item.texture);
    cache.clear(); requested.clear();
    for (auto& text : text_faces) {
        text.font.reset();
        if (text.coverage) FT_Done_Face(text.coverage);
        text = {}; // Releases the buffer after both users are gone.
    }
    if (face) FT_Done_Face(face);
    if (library) FT_Done_FreeType(library);
    face = nullptr; library = nullptr;
    std::vector<unsigned char>().swap(emoji_data);
}
static float ui_font_scale = 1.0f;
void rich_text_set_scale(float scale) { if (scale!=ui_font_scale) { widths.clear();run_widths.clear(); } ui_font_scale = scale; }
int rich_text_width(vita2d_pgf* font, float scale, const char* text) {
    performance::SlowScope slow(performance::SlowPhase::Ui,"text_width");
    return widths.get(text,reinterpret_cast<uintptr_t>(font),scale*ui_font_scale,[&]{return render(font,0,0,0,scale*ui_font_scale,text,false);});
}
void rich_text_draw(vita2d_pgf* font, int x, int baseline, unsigned int color, float scale, const char* text) {
    performance::SlowScope slow(performance::SlowPhase::Ui,"text_draw");
    render(font, x, baseline, color, scale * ui_font_scale, text, true);
}

vita2d_texture* load_webp(const char* path) {
    std::string bytes; // sealed files are decrypted here
    const bool read = vault::read_file(path, 4 * 1024 * 1024, bytes) && !bytes.empty();
    std::vector<unsigned char> data(bytes.begin(), bytes.end());
    int w = 0, h = 0;
    if (!read || !WebPGetInfo(data.data(), data.size(), &w, &h) || w <= 0 || h <= 0 || w > 1024 || h > 1024) return nullptr;
    vita2d_texture* texture = vita2d_create_empty_texture(w, h);
    if (!texture) return nullptr;
    int stride = vita2d_texture_get_stride(texture);
    auto* pixels = static_cast<unsigned char*>(vita2d_texture_get_datap(texture));
    if (!WebPDecodeRGBAInto(data.data(), data.size(), pixels, static_cast<size_t>(stride) * h, stride)) {
        vita2d_free_texture(texture); return nullptr;
    }
    vita2d_texture_set_filters(texture, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    return texture;
}
