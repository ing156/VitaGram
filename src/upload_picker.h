#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Chat attachment flow: type popup -> file browser -> review form (add more / send).
// Pure UI state; sending is done by the caller through TelegramClient::sendLocalFile.
// Photos and videos are sent compressed, everything else as unmodified documents.
class UploadPicker {
public:
    enum class Kind { Photo, Video, File };
    enum class Stage { Closed, Menu, Browse, Review };
    struct Entry { std::string name; bool dir = false; int64_t size = 0; uint64_t modified = 0; std::string date, label, full; };
    struct Item { std::string path, name; Kind kind = Kind::File; int64_t size = 0; };
    static constexpr size_t MaxItems = 10;

    Stage stage = Stage::Closed;
    Kind kind = Kind::Photo;       // type being browsed
    std::string path, error;
    std::vector<Entry> entries;
    std::vector<Item> items;       // collected attachments
    int selected = 0;              // row in the current stage
    bool show_hidden = false;
    std::string info;              // why the list is empty (raw entries vs. filtered)
    std::vector<int> history;      // selected row of every folder we descended from

    bool active() const { return stage != Stage::Closed; }
    void openMenu();
    void close() { stage = Stage::Closed; items.clear(); }
    void chooseKind(Kind k);       // Menu / Review -> Browse
    void reload();
    void enter();                  // X in Browse: open folder / add file
    void up();                     // O in Browse: parent folder, drive list, or back
    void toggleHidden();
    void refresh();
    // Review rows: items..., then "add photo", "add video", "add file", "send".
    int reviewRows() const { return (int)items.size() + 4; }
    bool reviewActivate();         // true when the batch was released for sending
    void removeSelected();         // Triangle in Review
    const Item* focusedItem() const { return selected >= 0 && selected < (int)items.size() ? &items[selected] : nullptr; }
    bool takeResult(std::vector<Item>& out);

    // Path shown/used for an entry (flat "latest" view entries carry their own full path).
    std::string fullPath(const Entry& e) const;
    static constexpr const char* LatestView = "@latest";

    static bool matches(Kind k, const std::string& name);
    static std::string formatSize(int64_t bytes);
private:
    bool has_result_ = false;
    std::vector<Item> result_;
    static bool exists(const std::string& directory);
    void addShortcuts();
    void scanLatest(const std::string& root, const std::string& directory, int depth);
    void log(const std::string& text) const;
};
