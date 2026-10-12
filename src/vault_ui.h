#pragma once
#include <string>
#include <vita2d.h>

// Screens of the data vault: first-run question, key entry with console buttons, unlock at start,
// the encrypt/decrypt migration, and the flows behind the Start-menu settings rows. Every screen
// owns the display and the controller while it runs (nothing else is started yet at launch).
namespace vault_ui {
struct Env {
    vita2d_pgf* font;
    bool dark;
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
// delete damaged ones. Safe while TDLib runs: recently changed files (maybe downloading) are skipped.
Outcome check_flow(const Env& env);
}
