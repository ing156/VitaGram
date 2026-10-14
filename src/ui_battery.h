#pragma once

// UI thread only. Load between frames; keeps only the active theme's atlas.
namespace ui_battery {
void load(int theme);
void unload();
// Actual device percentage; charging takes priority. False when the sprite is unavailable.
bool draw(int percent, bool charging, float x, float y, float size = 20);
}
