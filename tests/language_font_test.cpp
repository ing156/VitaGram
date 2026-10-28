// Exercise the actual renderer/loader with real FreeType faces and a host GPU stub.
#include "../src/media_render.cpp"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {
std::mutex worker_mutex;
std::condition_variable worker_cv;
bool worker_blocked = false;
int live_textures = 0;
vita2d_texture* button_texture = nullptr;
std::vector<unsigned> button_tints;
void block_worker(bool blocked) {
    { std::lock_guard<std::mutex> lock(worker_mutex); worker_blocked = blocked; }
    worker_cv.notify_all();
}
void wait_for_read() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!language_loader.done) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    rich_text_poll();
}
}
cpu_scheduler::Thread::Thread(Role) : id_(0) {
    std::unique_lock<std::mutex> lock(worker_mutex);
    worker_cv.wait(lock, [] { return !worker_blocked; });
}
cpu_scheduler::Thread::~Thread() = default;
bool ui_skin::has_buttons() { return false; }
bool ui_skin::button(int, Sprite& sprite) {
    if (!button_texture) return false;
    sprite = {};
    sprite.texture = button_texture;
    sprite.w = sprite.h = 48;
    sprite.vx = sprite.vy = 12;
    sprite.vw = sprite.vh = 24;
    return true;
}
bool vault::read_file(const std::string&, size_t, std::string&, bool (*)(void*), void*, Require) { return false; }

vita2d_texture* vita2d_create_empty_texture_format(unsigned w, unsigned h, unsigned) {
    ++live_textures;
    return new vita2d_texture{w, h, w * 4, std::vector<unsigned char>(w * h * 4)};
}
vita2d_texture* vita2d_create_empty_texture(unsigned w, unsigned h) { return vita2d_create_empty_texture_format(w, h, 0); }
vita2d_texture* vita2d_load_PNG_file(const char*) { return nullptr; }
void* vita2d_texture_get_datap(vita2d_texture* texture) { return texture->pixels.data(); }
unsigned vita2d_texture_get_stride(vita2d_texture* texture) { return texture->stride; }
unsigned vita2d_texture_get_width(const vita2d_texture* texture) { return texture->width; }
unsigned vita2d_texture_get_height(const vita2d_texture* texture) { return texture->height; }
void vita2d_texture_set_filters(vita2d_texture*, unsigned, unsigned) {}
void vita2d_free_texture(vita2d_texture* texture) { --live_textures; delete texture; }
void vita2d_wait_rendering_done() {}
void vita2d_draw_texture_tint_part_scale(const vita2d_texture*, float, float, float, float, float, float, float, float, unsigned tint) { button_tints.push_back(tint); }
void vita2d_draw_texture_part_scale(const vita2d_texture*, float, float, float, float, float, float, float, float) {}
void vita2d_draw_texture_scale(const vita2d_texture*, float, float, float, float) {}
void vita2d_draw_rectangle(float, float, float, float, unsigned) {}
void vita2d_pgf_draw_text(vita2d_pgf*, int, int, unsigned, float, const char*) {}
int vita2d_pgf_text_width(vita2d_pgf*, float, const char*) { return 0; }

int main(int argc, char** argv) {
    assert(argc == 2);
    // Reopening a menu starts at alpha zero; both icon sources must follow every fade frame.
    auto* icon = vita2d_create_empty_texture(48, 48);
    for (bool skinned : {false, true}) {
        button_texture = skinned ? icon : nullptr;
        controller_icons.fill(skinned ? nullptr : icon);
        for (int font = 0; font < 7; ++font) {
            rich_text_set_scale(0.7f + font * 0.1f);
            constexpr float scale = 0.62f;
            for (int i = 0; i < 9; ++i) {
                const char code[] = {char(0xee), char(0x80), char(0x80 + i), 0};
                const int width = rich_text_width(nullptr, scale, code);
                for (unsigned alpha : {0u, 128u, 255u, 0u, 255u}) {
                    button_tints.clear();
                    rich_text_draw(nullptr, 0, 30, (alpha << 24) | 0x123456u, scale, code);
                    assert(button_tints.size() == 1);
                    assert(button_tints[0] == ((alpha << 24) | 0xffffffu));
                    assert(rich_text_width(nullptr, scale, code) == width);
                }
            }
        }
    }
    button_texture = nullptr;
    controller_icons.fill(nullptr);
    vita2d_free_texture(icon);
    button_tints.clear();
    rich_text_set_scale(1.f);
    std::puts("Controller hints: atlas/PNG alpha, repeated fade resets, colors and widths at all seven font sizes passed");
    const std::string assets = argv[1];
    const std::string kr = assets + "/NotoSansKR-Regular.otf";
    const std::string sc = assets + "/NotoSansSC-Regular.otf";
    const std::string tc = assets + "/NotoSansTC-Regular.otf";
    assert(!FT_Init_FreeType(&library));
    assert(rich_text_japanese_font_status() == LanguageFontStatus::Loading);
    loader.adopted = true;
    loader.text_adopted = true;
    assert(rich_text_japanese_font_status() == LanguageFontStatus::Failed);
    std::vector<unsigned char> bytes;
    assert(read_file((assets + "/NotoSansJP-Regular.otf").c_str(), bytes));
    assert(adopt_text_face(3, std::move(bytes)));
    assert(rich_text_japanese_font_status() == LanguageFontStatus::Ready);
    assert(read_file((assets + "/NotoSans-Regular.ttf").c_str(), bytes));
    assert(adopt_text_face(0, std::move(bytes)));
    assert(rich_text_set_language_font(kr.c_str()));
    auto* original = text_faces[language_slot].coverage;
    assert(font_for(U'한') == text_faces[language_slot].font.get());
    rich_text_draw(nullptr, 0, 30, ~0u, 1, u8"한字 Latin");
    const auto revision = rich_text_revision();

    // A blocked disk read must not change the active face, layout or coverage.
    block_worker(true);
    assert(rich_text_prepare_language_font(sc.c_str()) == LanguageFontStatus::Loading);
    assert(rich_text_prepare_language_font(tc.c_str()) == LanguageFontStatus::Loading);
    assert(text_faces[language_slot].coverage == original);
    assert(rich_text_revision() == revision);
    block_worker(false);
    wait_for_read(); // SC result is stale; do not adopt it.
    assert(spare_language_path.empty());
    assert(text_faces[language_slot].coverage == original);
    assert(rich_text_prepare_language_font(tc.c_str()) == LanguageFontStatus::Loading);
    wait_for_read();
    assert(rich_text_prepare_language_font(tc.c_str()) == LanguageFontStatus::Ready);
    assert(text_faces[language_slot].coverage == original); // Ready is still not committed.
    assert(rich_text_set_language_font(tc.c_str()));
    assert(language_font_path == tc && spare_language_path == kr);
    assert(rich_text_revision() == revision + 1);
    assert(rich_text_prepare_language_font(kr.c_str()) == LanguageFontStatus::Ready);
    assert(!language_loader.started);
    assert(rich_text_set_language_font(kr.c_str()));
    assert(text_faces[language_slot].coverage == original); // Reused, including its atlas.

    // Both missing and corrupt assets leave the working font intact.
    for (const std::string& bad : {assets + "/missing-font.otf", assets + "/lang/en.lang"}) {
        assert(rich_text_prepare_language_font(bad.c_str()) == LanguageFontStatus::Loading);
        wait_for_read();
        assert(rich_text_prepare_language_font(bad.c_str()) == LanguageFontStatus::Failed);
        assert(text_faces[language_slot].coverage == original);
        assert(!rich_text_set_language_font(bad.c_str()));
        assert(text_faces[language_slot].coverage == original);
    }
    // Cancelling back to the active/default face also invalidates an in-flight result.
    block_worker(true);
    assert(rich_text_prepare_language_font(sc.c_str()) == LanguageFontStatus::Loading);
    assert(rich_text_prepare_language_font(nullptr) == LanguageFontStatus::Ready);
    assert(rich_text_set_language_font(nullptr));
    assert(!text_faces[language_slot].font && spare_language_path == kr);
    block_worker(false);
    wait_for_read();
    assert(spare_language_path == kr);
    assert(rich_text_prepare_language_font(kr.c_str()) == LanguageFontStatus::Ready);
    assert(rich_text_set_language_font(kr.c_str()));
    assert(font_for(U'한') == text_faces[language_slot].font.get());
    // User fonts are discovered independently of the current working directory and language choice.
    namespace fs = std::filesystem;
    const auto previous_directory = fs::current_path();
    char temp[] = "/tmp/vg-user-fonts-XXXXXX";
    const char* temporary = mkdtemp(temp); assert(temporary);
    fs::current_path(temporary);
    fs::create_directories("ux0:data/vitagram");
    assert(user_fonts::scan().empty());
    for (const char* invalid : {"", ".ttf", "../font.ttf", "a/b.ttf", "a\\b.ttf", "a:font.ttf", "font.woff", " font.ttf", "\xff.ttf"})
        assert(!user_fonts::valid_filename(invalid));
    assert(user_fonts::valid_filename(u8"Мой шрифт.TTF"));
    const auto user_path = user_fonts::path("Coptic.TTF");
    fs::copy_file(assets + "/NotoSansCoptic-Regular.ttf", user_path);
    std::ofstream(user_fonts::path("empty.ttf"));
    std::ofstream(user_fonts::path("large.otf")) << "x";
    fs::resize_file(user_fonts::path("large.otf"), user_fonts::max_bytes + 1);
    fs::create_directory(user_fonts::path("directory.ttf"));
    fs::create_symlink(fs::absolute(user_path), user_fonts::path("link.ttf"));
    assert(user_fonts::scan() == std::vector<std::string>{"Coptic.TTF"});
    auto wait_custom = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!custom_loader.done) { assert(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
        rich_text_poll();
    };
    const auto custom_revision = rich_text_revision();
    assert(rich_text_prepare_custom_font("Coptic.TTF") == LanguageFontStatus::Loading);
    wait_custom();
    assert(rich_text_prepare_custom_font("Coptic.TTF") == LanguageFontStatus::Ready);
    assert(!text_faces[custom_slot].font); // Loading alone never changes rendering.
    assert(rich_text_set_custom_font("Coptic.TTF"));
    assert(rich_text_revision() == custom_revision + 1);
    auto* user_face = text_faces[custom_slot].font.get();
    assert(font_for(0x2c80) == user_face);
    assert(!FT_Get_Char_Index(text_faces[custom_slot].coverage, U'Я'));
    assert(font_for(U'Я') == text_faces[0].font.get());
    assert(font_for(U'한') == text_faces[language_slot].font.get());
    rich_text_draw(nullptr, 0, 30, ~0u, 1, u8"Ⲁ Я 한");
    for (const char* bad : {"missing.ttf", "large.otf", "link.ttf", "directory.ttf"}) {
        assert(!rich_text_set_custom_font(bad));
        assert(text_faces[custom_slot].font.get() == user_face);
    }
    std::ofstream(user_fonts::path("corrupt.ttf")) << "not a font";
    assert(rich_text_prepare_custom_font("corrupt.ttf") == LanguageFontStatus::Loading);
    wait_custom();
    assert(rich_text_prepare_custom_font("corrupt.ttf") == LanguageFontStatus::Failed);
    assert(!rich_text_set_custom_font("corrupt.ttf"));
    assert(text_faces[custom_slot].font.get() == user_face);
    assert(rich_text_prepare_custom_font("../escape.ttf") == LanguageFontStatus::Failed);
    block_worker(true);
    assert(rich_text_prepare_custom_font("corrupt.ttf") == LanguageFontStatus::Failed);
    assert(rich_text_prepare_custom_font("") == LanguageFontStatus::Ready);
    assert(rich_text_prepare_custom_font("corrupt.ttf") == LanguageFontStatus::Loading);
    assert(rich_text_prepare_custom_font("") == LanguageFontStatus::Ready);
    block_worker(false); wait_custom(); // Cancelled result cannot replace the working face.
    assert(text_faces[custom_slot].font.get() == user_face);
    assert(rich_text_set_custom_font(""));
    assert(font_for(0x2c80) != user_face);
    assert(rich_text_set_custom_font("Coptic.TTF")); // Cached reuse, then return to default.
    assert(text_faces[custom_slot].font.get() == user_face);
    assert(rich_text_set_custom_font(""));
    for (int i = 0; i < 70; ++i) std::ofstream(user_fonts::path("font" + std::to_string(i) + ".otf")) << "x";
    const auto many = user_fonts::scan();
    assert(many.size() == user_fonts::max_fonts && std::is_sorted(many.begin(), many.end()));
    fs::current_path(previous_directory);
    fs::remove_all(temporary);
    std::puts("User fonts: discovery, filename validation, size limit, Unicode priority, built-in fallback, async cancellation and safe failure passed");
    // Shutdown joins a still-running request and releases all retained atlases/faces.
    assert(rich_text_prepare_language_font(sc.c_str()) == LanguageFontStatus::Loading);
    rich_text_shutdown();
    assert(!language_loader.started && !library && live_textures == 0);
    std::puts("Language fonts: stale reads, cancellation, safe failure, cached reuse, coverage and shutdown passed");
}
