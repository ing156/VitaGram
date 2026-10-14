// Exercise the actual renderer/loader with real FreeType faces and a host GPU stub.
#include "../src/media_render.cpp"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
std::mutex worker_mutex;
std::condition_variable worker_cv;
bool worker_blocked = false;
int live_textures = 0;
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
bool ui_skin::button(int, Sprite&) { return false; }
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
void vita2d_draw_texture_tint_part_scale(const vita2d_texture*, float, float, float, float, float, float, float, float, unsigned) {}
void vita2d_draw_texture_part_scale(const vita2d_texture*, float, float, float, float, float, float, float, float) {}
void vita2d_draw_texture_scale(const vita2d_texture*, float, float, float, float) {}
void vita2d_draw_rectangle(float, float, float, float, unsigned) {}
void vita2d_pgf_draw_text(vita2d_pgf*, int, int, unsigned, float, const char*) {}
int vita2d_pgf_text_width(vita2d_pgf*, float, const char*) { return 0; }

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string assets = argv[1];
    const std::string kr = assets + "/NotoSansKR-Regular.otf";
    const std::string sc = assets + "/NotoSansSC-Regular.otf";
    const std::string tc = assets + "/NotoSansTC-Regular.otf";
    assert(!FT_Init_FreeType(&library));
    assert(rich_text_japanese_font_status() == LanguageFontStatus::Loading);
    loader.adopted = true;
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
    // Shutdown joins a still-running request and releases all retained atlases/faces.
    assert(rich_text_prepare_language_font(sc.c_str()) == LanguageFontStatus::Loading);
    rich_text_shutdown();
    assert(!language_loader.started && !library && live_textures == 0);
    std::puts("Language fonts: stale reads, cancellation, safe failure, cached reuse, coverage and shutdown passed");
}
