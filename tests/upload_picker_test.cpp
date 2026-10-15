#include "upload_picker.h"
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/apputil.h>
#include <psp2/rtc.h>
#include <cassert>
#include <cstdio>
#include <map>

// Directory fixtures exercise the real reload/navigation code without card I/O.
static std::map<std::string, std::vector<SceIoDirent>> directories;
static std::string current_directory;
static size_t directory_row = 0;
SceUID sceIoDopen(const char* path) { current_directory = path; directory_row = 0; return 1; }
int sceIoDread(SceUID, SceIoDirent* out) {
    const auto it = directories.find(current_directory);
    if (it == directories.end() || directory_row == it->second.size()) return 0;
    *out = it->second[directory_row++];
    return 1;
}
int sceIoDclose(SceUID) { return 0; }
int sceIoGetstat(const char*, SceIoStat*) { return -1; }
int sceAppUtilSystemParamGetInt(unsigned int, int*) { return -1; }
int sceRtcGetTick(const SceDateTime*, SceRtcTick*) { return -1; }
int sceRtcSetTick(SceDateTime*, const SceRtcTick*) { return -1; }
int sceRtcConvertUtcToLocalTime(const SceRtcTick*, SceRtcTick*) { return -1; }

int main() {
    using Kind = UploadPicker::Kind;
    using Stage = UploadPicker::Stage;
    for (Kind kind : {Kind::Photo, Kind::Video, Kind::File}) {
        UploadPicker picker;
        picker.openMenu();
        picker.chooseKind(kind);
        picker.finishSelection();
        assert(picker.stage == Stage::Browse);
        picker.path = "ux0:picture";
        for (int i = 0; i < 11; ++i)
            picker.entries.push_back({"file" + std::to_string(i), false, i + 100});
        // Remove drive shortcuts supplied by chooseKind.
        picker.entries.erase(picker.entries.begin(), picker.entries.end() - 11);
        for (int i = 0; i < 10; ++i) {
            picker.selected = i;
            picker.enter();
            assert(picker.stage == Stage::Browse && picker.selected == i);
            assert(picker.isMarked(picker.entries[i]));
            assert(picker.items.back().kind == kind);
        }
        picker.selected = 10;
        picker.enter();
        assert(picker.items.size() == 10 && !picker.isMarked(picker.entries[10]));
        picker.selected = 3;
        picker.enter();
        assert(picker.items.size() == 9 && !picker.isMarked(picker.entries[3]));
        picker.selected = 10;
        picker.enter();
        assert(picker.items.size() == 10 && picker.isMarked(picker.entries[10]));
        // Marks use full paths across folders and the flat latest-screenshot view.
        const auto first = picker.items.front();
        picker.path = "ux0:other";
        assert(!picker.isMarked(picker.entries[0]));
        picker.entries[0].full = first.path;
        assert(picker.isMarked(picker.entries[0]));
        picker.selected = 0;
        picker.enter();
        assert(picker.items.size() == 9 && !picker.isMarked(picker.entries[0]));
        picker.enter();
        assert(picker.items.size() == 10);
        picker.entries[0].dir = true;
        assert(!picker.isMarked(picker.entries[0]));
        picker.entries[0].dir = false;
        picker.refresh();
        assert(picker.items.size() == 10 && picker.stage == Stage::Browse);
        picker.toggleHidden();
        assert(picker.show_hidden && picker.items.size() == 10);
        // Square reviews without producing an outgoing batch.
        picker.finishSelection();
        assert(picker.stage == Stage::Review && picker.selected == 0);
        std::vector<UploadPicker::Item> result;
        assert(!picker.takeResult(result));
        assert(picker.focusedItem() == nullptr); // first row is Send
        picker.selected = 1;
        assert(picker.focusedItem() == &picker.items[0]);
        picker.selected = 10;
        assert(picker.focusedItem() == &picker.items[9]);
        if (kind == Kind::Video) assert(picker.sendBatch()); // shortcut from a file row
        else {
            picker.selected = 0;
            assert(picker.reviewActivate()); // X on the top Send row
        }
        assert(picker.takeResult(result) && result.size() == 10);
        assert(!picker.takeResult(result));
        assert(picker.stage == Stage::Closed && picker.items.empty());
    }
    UploadPicker picker;
    picker.chooseKind(Kind::Photo);
    picker.path = "ux0:picture";
    picker.entries = {{"image.png", false, 123}};
    picker.enter();
    picker.entries = {{"nested", true}};
    picker.enter();
    assert(picker.path == "ux0:picture/nested" && picker.items.size() == 1);
    picker.up();
    assert(picker.path == "ux0:picture" && picker.items.size() == 1);
    picker.close();
    assert(picker.items.empty());
    // Offset file rows must remove/preview the right item, never the Send row.
    UploadPicker review;
    review.stage = Stage::Review;
    review.items = {{"a", "a", Kind::File, 1}, {"b", "b", Kind::Video, 2}};
    review.selected = 0;
    review.removeSelected();
    assert(review.items.size() == 2 && !review.focusedItem());
    review.selected = 1;
    review.removeSelected();
    assert(review.items.size() == 1 && review.focusedItem()->path == "b");
    assert(!review.reviewActivate()); // file row does not send
    std::vector<UploadPicker::Item> outgoing;
    assert(!review.takeResult(outgoing));
    review.selected = review.reviewRows() - 1; // shortcut also works on Add file
    assert(!review.focusedItem());
    assert(review.sendBatch() && review.takeResult(outgoing));
    assert(outgoing.size() == 1 && outgoing[0].path == "b");
    assert(!review.sendBatch() && !review.takeResult(outgoing));
    review.stage = Stage::Review;
    assert(!review.sendBatch()); // an empty batch is never released
    for (int k = 0; k < 3; ++k) {
        review.stage = Stage::Review;
        review.items = {{"kept", "kept", Kind::File, 1}};
        review.selected = 2 + k; // Send, one file, then the three Add rows
        assert(!review.reviewActivate());
        assert(review.stage == Stage::Browse && review.kind == (Kind)k);
        assert(review.items.size() == 1 && !review.sendBatch());
    }
    using Sort = UploadPicker::Sort;
    UploadPicker sorted;
    sorted.chooseKind(Kind::File);
    sorted.path = "ux0:data";
    sorted.entries = {{"bravo", false, 10, 30}, {"Alpha", false, 30, 10},
                      {"charlie", false, 20, 20}, {"alpha", false, 15, 10}};
    sorted.selected = 0;
    sorted.enter();
    const std::vector<std::vector<std::string>> orders = {
        {"Alpha", "alpha", "charlie", "bravo"}, // oldest
        {"Alpha", "alpha", "bravo", "charlie"}, // name ascending (case insensitive)
        {"charlie", "bravo", "Alpha", "alpha"}, // name descending
        {"bravo", "alpha", "charlie", "Alpha"}, // size ascending
        {"Alpha", "charlie", "alpha", "bravo"}, // size descending
        {"bravo", "charlie", "Alpha", "alpha"}  // newest, wrapped
    };
    for (const auto& order : orders) {
        sorted.changeSort(1);
        for (size_t i = 0; i < order.size(); ++i) assert(sorted.entries[i].name == order[i]);
        assert(sorted.entries[sorted.selected].name == "bravo");
        assert(sorted.items.size() == 1 && sorted.isMarked(sorted.entries[sorted.selected]));
    }
    assert(sorted.sort == Sort::Newest);
    sorted.changeSort(-1);
    assert(sorted.sort == Sort::SizeDesc);
    sorted.changeSort(1);
    assert(sorted.sort == Sort::Newest);
    // Same basename in two screenshot folders must retain the exact cursor/mark.
    sorted.path = UploadPicker::LatestView;
    sorted.entries = {{"same.png", false, 10, 1, "", "", "ux0:picture/a/same.png"},
                      {"same.png", false, 20, 2, "", "", "ux0:picture/b/same.png"}};
    sorted.selected = 1;
    sorted.enter();
    sorted.changeSort(1);
    assert(sorted.fullPath(sorted.entries[sorted.selected]) == "ux0:picture/b/same.png");
    assert(sorted.isMarked(sorted.entries[sorted.selected]));
    assert(!sorted.isMarked(sorted.entries[1 - sorted.selected]));
    // Returning from a child must find its new row after changing the sort there.
    SceIoDirent child{}, file{};
    std::snprintf(child.d_name, sizeof(child.d_name), "child");
    child.d_stat.st_mode = SCE_S_IFDIR;
    std::snprintf(file.d_name, sizeof(file.d_name), "aaa");
    directories["ux0:data"] = {file, child};
    sorted.path = "ux0:data";
    sorted.sort = Sort::NameAsc;
    sorted.reload();
    assert(sorted.entries[1].name == "child");
    sorted.selected = 1;
    sorted.enter();
    sorted.changeSort(1); // name descending while inside child
    sorted.up();
    assert(sorted.path == "ux0:data" && sorted.entries[sorted.selected].name == "child");
    assert(sorted.selected == 0);
    sorted.refresh();
    assert(sorted.sort == Sort::NameDesc && sorted.entries[sorted.selected].name == "child");
    sorted.entries.clear();
    sorted.changeSort(-1);
    assert(sorted.sort == Sort::NameAsc && sorted.entries.empty());
    sorted.finishSelection();
    const auto before = sorted.sort;
    sorted.changeSort(1);
    assert(sorted.sort == before); // sort controls belong only to browsing
    // The photo shortcut stays first through every sort, refresh and return.
    UploadPicker photos;
    photos.chooseKind(Kind::Photo);
    assert(photos.entries.front().name == UploadPicker::LatestView);
    photos.selected = 1;
    const std::string focused_folder = photos.fullPath(photos.entries[1]);
    for (int i = 0; i < (int)Sort::Count; ++i) {
        photos.changeSort(1);
        assert(photos.entries.front().name == UploadPicker::LatestView);
        assert(photos.fullPath(photos.entries[photos.selected]) == focused_folder);
        photos.refresh();
        assert(photos.entries.front().name == UploadPicker::LatestView);
    }
    photos.selected = 0;
    photos.enter();
    assert(photos.path == UploadPicker::LatestView);
    photos.changeSort(-1);
    photos.up();
    assert(photos.path.empty() && photos.selected == 0);
    assert(photos.entries.front().name == UploadPicker::LatestView);
    std::puts("upload picker: selection, sorting, cursor identity, navigation and explicit send: PASS");
}
