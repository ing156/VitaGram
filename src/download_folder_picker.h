#pragma once
#include "i18n.h"
#include <string>
#include <vector>

class DownloadFolderPicker {
public:
    bool active = false;
    std::string path; i18n::Text error;
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
