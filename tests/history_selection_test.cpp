#include "history_selection.h"
#include <cassert>
#include <iostream>

static MessageItem message(int64_t id, int64_t album = 0, bool photo = false) {
    MessageItem item; item.id = id; item.media_album_id = album;
    if (photo) item.photos.push_back({});
    else if (album) item.video = true;
    return item;
}
int main() {
    // Mixed album: the first photo and the two videos share an album ID but
    // occupy different rows. A frame refresh must preserve all three selections.
    std::vector<MessageItem> rows = {message(100, 7, true), message(110, 7),
        message(120, 7), message(130, 7, true), message(200)};
    for (int i = 0; i < (int)rows.size(); ++i)
        assert(HistorySelection::capture(rows, i).find(rows) == i);

    // Reproduce pressing down across an album, with pump/selection restore between
    // every button press: it must reach newer messages, not reset to the first row.
    int selected = 0;
    for (int target = 1; target < (int)rows.size(); ++target) {
        ++selected;
        selected = HistorySelection::capture(rows, selected).find(rows);
        assert(selected == target);
    }
    for (int target = (int)rows.size()-2; target >= 0; --target) {
        --selected;
        selected = HistorySelection::capture(rows, selected).find(rows);
        assert(selected == target);
    }

    auto video = HistorySelection::capture(rows, 2);
    auto photo = HistorySelection::capture(rows, 3);
    rows.insert(rows.begin(), message(50));
    assert(video.find(rows) == 3 && photo.find(rows) == 4);
    rows.push_back(message(210));
    assert(video.find(rows) == 3 && photo.find(rows) == 4);

    // Older photos on a new page merge into the selected photo album, changing
    // its representative ID. Prefer the nearest photo segment, never a video.
    auto merged = HistorySelection::capture(std::vector<MessageItem>{message(140, 7, true)}, 0);
    assert(merged.find(rows) == 4); // Photo 130, not photo 100 or videos 110/120.
    rows[4].id = 125;
    assert(merged.find(rows) == 4);

    // A deleted video must not fall back to another row of its album.
    rows.erase(rows.begin()+3);
    assert(video.find(rows) == -1);
    assert(HistorySelection::capture(rows, -1).find(rows) == -1);
    assert(HistorySelection::capture(rows, 999).find(rows) == -1);
    assert(photo.find({}) == -1);
    assert(merged.find({message(90, 8, true), message(110, 7)}) == -1);
    assert(merged.find({message(150, 7, true)}) == -1);
    std::cout << "History selection: mixed albums, up/down, pagination, photo merges and deletions PASS\n";
}
