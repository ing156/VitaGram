#pragma once
#include <string>

// Installs the extracted package in `package_dir` through the system package promoter (the same
// calls VitaShell's installer makes: PAF, ScePromoterUtil, promote with RIF). The title being replaced
// must not be running - the console refuses to overwrite a live application, which is why the update is
// installed by the separate VitaGram Updater app (src/updater_app). Use a shallow folder (ux0:data/<name>).
// Vita only; on other platforms it always fails. `detail` gets the English reason for diagnostics.
// `installed_eboot` / `expected_size`: after the promoter is idle again, an installed eboot.bin of that size
// counts as success even if the promoter's own result code is an error (the result alone proved unreliable;
// PSVitaAlive judges by the installed files too). Pass nullptr to rely on the result code.
bool promote_package(const std::string& package_dir, std::string& detail,
                     const char* installed_eboot = nullptr, long long expected_size = -1);

// Removes an installed title (its LiveArea bubble and files) through the promoter. The title must not be
// running. Vita only; on other platforms it always fails.
bool delete_package(const char* title_id, std::string& detail);
