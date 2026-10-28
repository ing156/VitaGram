#pragma once
#include <string>
#include <vector>
#include <vita2d.h>

// Screens of the data vault: first-run question, key entry with console buttons, unlock at start,
// the encrypt/decrypt migration, and the flows behind the Start-menu settings rows. Every screen
// owns the display and the controller while it runs (nothing else is started yet at launch).
namespace vault_ui {
struct Env {
    vita2d_pgf* font;
    bool dark;
    int theme = -1; // Default keeps existing callers compatible; main passes the saved theme ID.
    int scroll_speed = 0;
    std::vector<std::string> keyboard_languages;
    int keyboard_split = 1;
    // Optional: run once per drawn frame (not while a key is derived) by the settings flows, so the
    // caller keeps pumping its event queues while a vault screen owns the display.
    void (*tick)(void*) = nullptr;
    void* tick_context = nullptr;
};

// Launch gate: asks once whether to encrypt, unlocks an existing vault. Returns when the app may
// continue (vault off, or unlocked).
void run_gate(const Env& env);
// Seals (or unseals, while switching off) files left over from before, if the vault says so.
void run_migration(const Env& env);
// A full-screen message acknowledged with {cross} (same look as the vault screens).
void show_notice(const Env& env, const std::string& text);

enum class Outcome { Done, Cancelled, Failed };
Outcome enable_flow(const Env& env);   // new key twice -> vault on
Outcome disable_flow(const Env& env);  // current key -> vault switching off
Outcome change_flow(const Env& env);   // current key, new key twice
// Lists how many files are sealed, seals the plain ones on request, verifies sealed files and can
// delete damaged ones. Safe while TDLib runs: recently changed files (maybe downloading) and TDLib's
// temp directory are skipped, and the background sealer is held off while files are converted.
Outcome check_flow(const Env& env);

// Lock while the app runs (auto-lock after inactivity, sleep or the PS button): asks for the key until
// it verifies. Nothing is closed or wiped - TDLib, downloads and the cache keep working - and there is
// no "forgot the key" path. `tick` runs once per drawn frame (not while the key is being derived) so
// the caller can keep pumping its event queues.
void relock(const Env& env, void (*tick)(void*), void* context);
}
