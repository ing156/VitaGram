# Instructions for agents

- Before starting work, read the root `README.md` and follow its project requirements and build instructions.
- When a build is required, build the project in `build-modern` using the maximum number of available CPU cores: `cmake --build build-modern --parallel "$(nproc)"`. If configuration is required, use the options documented in `README.md`. The build output is `build-modern/vitagram.vpk`. Documentation-only changes do not require a build.
- Unless the user requests otherwise, increment the final version component by one only when preparing a new build that includes changes to the project code, for example, `0.36.20` → `0.36.21`. This is what a "minor" version bump means in this project. Do not change the version for documentation-only changes or rebuilds without code changes; explicit user instructions about the version take precedence.
- Keep `VITAGRAM_VERSION` (for example, `0.36.21`) and `VITA_VERSION` (for example, `36.21`) in `CMakeLists.txt` consistent with the current version information in `README.md`.
- Whenever a version is added or a fix is made to a version during a chat, append a brief description of each specific change or fix to the shared `release/patchnote` file, one line per change, prefixed with the affected version (for example, `0.37.33: Fixed scrolling in the chat list`). Create the directory and file if missing, preserve existing entries, and record only changes made in the current chat.

## UI rules

- In choice grids, keep navigation focus and the saved selection independent: focus uses an outline; a saved selection uses a fill plus a graphical check badge. All four combinations must remain distinguishable without relying on color alone. Do not use an emoji-font glyph for a UI check mark.
- Center emoji and image previews in the same fixed visual area, reserve a separate line for counters, and keep badges clear of both. Fit the complete emoji sequence; never truncate it as ordinary text.
- Use the same grid geometry for rendering and touch hit testing. Gaps, unused cells and space outside the grid must not activate an item.
- Modal pickers show their own return control and relevant hints; hide and disable underlying chat actions. Verify empty/loading states, chosen items with and without focus, the last page, light/dark/custom palettes and font sizes. Report separately what was tested in code and what still needs a physical Vita check.
- Semi-transparent rounded fills must cover each point once; verify geometry coverage rather than only successful compilation.
