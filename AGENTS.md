# Instructions for agents

- When a build is required, build the project in `build-modern` using the maximum number of available CPU cores: `cmake --build build-modern --parallel "$(nproc)"`. If configuration is required, use `cmake -S . -B build-modern -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DTD_ENABLE_TESTS=OFF`. Dependency setup and troubleshooting are in `docs/building.md`. The build output is `build-modern/vitagram.vpk`. Documentation-only changes do not require a build.
- Unless the user requests otherwise, increment the final version component by one only when preparing a new build that includes changes to the project code, for example, `0.36.20` → `0.36.21`. This is what a "minor" version bump means in this project. Do not change the version for documentation-only changes or rebuilds without code changes; explicit user instructions about the version take precedence.
- Keep `VITAGRAM_VERSION` (for example, `0.36.21`) and `VITA_VERSION` (for example, `36.21`) in `CMakeLists.txt` consistent with the current version information in `README.md`.
- Whenever a version is added or a fix is made to a version during a chat, append a brief description of each specific change or fix to the shared `release/patchnote` file, one line per change, prefixed with the affected version (for example, `0.37.33: Fixed scrolling in the chat list`). Create the directory and file if missing, preserve existing entries, and record only changes made in the current chat.

## Development

- Inspect the working tree before editing. Preserve existing user changes, including changes inside submodules; do not reset or overwrite them to make a build pass.
- Use C++17 and the existing project helpers and patterns. Main UI: `src/main.cpp`; Telegram: `src/telegram_client.*`; playback: `src/animation_player.*` and `src/voice_player.*`; caches: `src/ui_cache.*` and `src/media_cache.*`; encryption: `src/vault*`; localization: `src/i18n.*`.
- Keep network, storage and media work off the UI thread. Bound queues, caches and buffers for Vita memory limits; preserve cancellation and reject stale asynchronous results.
- Keep API credentials in ignored `src/config.local.h`, using `src/config.local.h.example` as the template. Never commit credentials, account databases, keys, private media or diagnostic dumps; do not print their contents in reports.
- Dependencies are pinned submodules. Keep Vita adaptations in the existing `scripts/*-vita.patch` workflow. Rebuild the affected dependency and reconfigure when its configuration changes; do not substitute host libraries for Vita libraries.
- Add localization keys to every `assets/lang/*.lang` catalog. Preserve button tokens and numbered placeholders; run `bash tests/run-i18n-tests.sh` after translation changes.
- Run the relevant regression checks from `tests/README.md` for changed behavior. Keep necessary text fixtures in `tests/fixtures/`; keep private media and diagnostic output in ignored local directories. Report check failures and unavailable prerequisites.
- Host tests do not establish Vita rendering, sound, camera, microphone, storage throughput or installation behavior. Clearly separate completed host checks from physical-device checks still required.
- Keep the root README brief. Put user instructions and feature details in the relevant `docs/*.md` page; build instructions belong in `docs/building.md`, release signing in `RELEASING.md`. Update links when moving documentation. Preserve third-party licenses and notices when changing packaged assets or dependencies.

## UI rules

- In choice grids, keep navigation focus and the saved selection independent: focus uses an outline; a saved selection uses a fill plus a graphical check badge. All four combinations must remain distinguishable without relying on color alone. Do not use an emoji-font glyph for a UI check mark.
- Center emoji and image previews in the same fixed visual area, reserve a separate line for counters, and keep badges clear of both. Fit the complete emoji sequence; never truncate it as ordinary text.
- Use the same grid geometry for rendering and touch hit testing. Gaps, unused cells and space outside the grid must not activate an item.
- Modal pickers show their own return control and relevant hints; hide and disable underlying chat actions. Verify empty/loading states, chosen items with and without focus, the last page, light/dark/custom palettes and font sizes. Report separately what was tested in code and what still needs a physical Vita check.
- Semi-transparent rounded fills must cover each point once; verify geometry coverage rather than only successful compilation.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

When the user types `/graphify`, use the installed graphify skill or instructions before doing anything else.

Rules:
- When graphify-out/graph.json exists, run `graphify query "<question>"` before your first grep, search or source-file read for a codebase question, and put this rule in every subagent prompt that explores code. Read raw files only for lines the graph or an earlier read already located. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- Dirty graphify-out/ files are expected after hooks or incremental updates; dirty graph files are not a reason to skip graphify. Only skip graphify if the task is about stale or incorrect graph output, or the user explicitly says not to use it.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
- A committed graphify-out/graph.json is rewritten whole on every build, so a large diff to it in a pull request is expected. Do not flag it as churn or suggest skipping the graph rebuild or its hooks.
