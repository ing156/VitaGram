#include "media_trash.h"
#include "config.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <sys/stat.h>

static bool exists(const std::string& path) { struct stat info{}; return stat(path.c_str(), &info) == 0; }
static void fill(const std::string& dir, int count) {
    mkdir(dir.c_str(), 0777);
    for (int i = 0; i < count; ++i) {
        FILE* f = std::fopen((dir + "/file_" + std::to_string(i) + ".jpg").c_str(), "wb");
        assert(f); std::fputs("x", f); std::fclose(f);
    }
}
static void sweep(media_trash::Sweeper& sweeper) {
    int64_t now = 0;
    for (int i = 0; i < 100000 && !sweeper.done(); ++i) { sweeper.step(now); now += 10000000; }
    assert(sweeper.done());
}
int main() {
    mkdir("ux0:data", 0777); mkdir("ux0:data/vitagram", 0777);
    mkdir(VITAGRAM_DB_DIR, 0777); mkdir(VITAGRAM_FILES_DIR, 0777);
    const std::string photos = VITAGRAM_DB_DIR "/profile_photos", stickers = VITAGRAM_DB_DIR "/stickers";
    fill(photos, 70); fill(stickers, 5); fill(VITAGRAM_FILES_DIR "/photos", 3);

    // First start of this layout: bloated media directories are swapped out,
    // stickers stay, and the move happens only once.
    assert(media_trash::rotate() == 2);
    assert(!exists(photos) && exists(photos + ".trash1") && exists(stickers + "/file_0.jpg"));
    fill(photos, 1);
    assert(media_trash::rotate() == 0 && exists(photos + "/file_0.jpg"));

    // The trash is emptied in batches (more than one readdir batch) and removed.
    { media_trash::Sweeper sweeper; sweep(sweeper); }
    assert(!exists(photos + ".trash1") && !exists(VITAGRAM_FILES_DIR "/photos.trash1"));
    assert(exists(photos + "/file_0.jpg") && !exists(photos + ".rotate"));

    // A directory above its limit is flagged and swapped at the next start.
    fill(photos, 2001);
    { media_trash::Sweeper sweeper; sweep(sweeper); }
    assert(exists(photos + ".rotate") && !exists(stickers + ".rotate"));
    assert(media_trash::rotate() == 1);
    assert(!exists(photos) && exists(photos + ".trash1") && !exists(photos + ".rotate"));
    { media_trash::Sweeper sweeper; sweep(sweeper); }
    assert(!exists(photos + ".trash1"));

    // Layout 2 (bucketed TDLib names): flat photo directories rotate once, stickers never.
    std::remove(VITAGRAM_DB_DIR "/media-layout-2");
    fill(photos, 3); fill(stickers, 2);
    assert(media_trash::rotate() == 1 && !exists(photos) && exists(stickers + "/file_0.jpg"));
    fill(photos, 1);
    assert(media_trash::rotate() == 0 && exists(photos + "/file_0.jpg"));
    { media_trash::Sweeper sweeper; sweep(sweeper); }

    // Layout 3 (bucketed stickers): the flat sticker directory rotates once on upgrade.
    std::remove(VITAGRAM_DB_DIR "/media-layout-3");
    fill(photos, 1);
    assert(media_trash::rotate() == 1 && !exists(stickers) && exists(stickers + ".trash1"));
    assert(exists(photos + "/file_0.jpg"));
    fill(stickers, 1);
    assert(media_trash::rotate() == 0 && exists(stickers + "/file_0.jpg"));
    { media_trash::Sweeper sweeper; sweep(sweeper); }
    std::puts("Media trash: one-time rotation, batched sweep, limit flag and rotation passed");
}
