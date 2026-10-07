#pragma once
#include <string>
#include <vector>

class DownloadFolderPicker {
public:
    bool active = false;
    std::string path, error;
    std::vector<std::string> folders;
    int selected = 0;

    void open(const std::string& preferred);
    void reload();
    void enter();
    void up();
    bool create(const std::string& name);
private:
    static bool exists(const std::string& directory);
};
