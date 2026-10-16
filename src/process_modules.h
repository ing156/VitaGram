#pragma once
#include <string>
#include <vector>

// Modules loaded into VitaGram's own process from a writable partition: user plugins taiHEN injects
// through the *ALL or *VITAGRM01 sections of tai/config.txt. Such code runs with VitaGram's rights -
// it can hook the controller and read the vault key as it is entered, and read the decrypted session
// from memory - so the key screens name them before the key is typed.
//
// System modules come from the read-only vs0:/os0: partitions and VitaGram's own from app0:. Kernel
// plugins (*KERNEL) are invisible from here and can read any process; no user-mode check helps there.
namespace process_modules {
constexpr char kTitleId[] = "VITAGRM01";

// `path` of a module that is neither the system's nor VitaGram's own (installed as `title_id`).
bool foreign_path(const std::string& path, const std::string& title_id);
// Names of foreign modules in this process; empty on the host build.
std::vector<std::string> foreign(const std::string& title_id);
// "a, b, c +2" for display.
std::string summary(const std::vector<std::string>& names, size_t shown = 3);
}
