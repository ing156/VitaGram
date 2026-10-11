# Instructions for agents

- Before starting work, read the root `README.md` and follow its project requirements and build instructions.
- Build the project in `build-modern` using the maximum number of available CPU cores: `cmake --build build-modern --parallel "$(nproc)"`. If configuration is required, use the options documented in `README.md`. The build output is `build-modern/vitagram.vpk`.
- Unless the user requests otherwise, increment the final version component by one when preparing a new build, for example, `0.36.20` → `0.36.21`. This is what a "minor" version bump means in this project.
- Keep `VITAGRAM_VERSION` (for example, `0.36.21`) and `VITA_VERSION` (for example, `36.21`) in `CMakeLists.txt` consistent with the current version information in `README.md`.
