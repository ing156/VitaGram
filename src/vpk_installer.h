#pragma once
#include <string>
// Native Vita dialogs. True means the updater owns the staged VitaGram package:
// the caller must exit without deleting staging or launching itself. Saved VPKs are preserved.
bool install_downloaded_vpk(const std::string& path);
