# Repository Guidelines

## Project Structure & Module Organization
- `tridigenerator_player/Src/`: native C++ gameplay and rendering code (Core, Components, Systems, States).
- `tridigenerator_player/java/`: Android Java entry point (`MainActivity.java`).
- `tridigenerator_player/assets/`: runtime assets bundled into the APK.
- `tridigenerator_player/res/`: Android resources (strings, layouts, etc.).
- `tridigenerator_player/Projects/Android/`: Gradle Android project and wrappers.
- `dependencies/`: third-party libraries (Meta SampleXrFramework, OpenXR headers, stb, etc.).
- `tridigenerator_player/Docs/`: generated Doxygen HTML/LaTeX output.

## Build, Test, and Development Commands
- `./gradlew :tridigenerator_player:assembleDebug` — builds the Android debug APK from the repo root.
- `tridigenerator_player/Projects/Android/gradlew assembleDebug` — same build, run from the Android project folder.
- `python tridigenerator_player/Projects/Android/build.py` — runs the Oculus SDK build wrapper used by Meta samples.
- `cd tridigenerator_player && doxygen Doxyfile` — regenerates native code docs in `tridigenerator_player/Docs/html/`.

## Coding Style & Naming Conventions
- C++ is built with C++17 (`CMakeLists.txt`); Java uses toolchain 17.
- Indentation: 4 spaces; keep braces and spacing consistent with existing files in `tridigenerator_player/Src/`.
- Naming: files and types use PascalCase (e.g., `FrameLoaderSystem.cpp`, `TDGenPlayerApp`).
- Prefer small, focused systems/components and mirror the ECS folder layout (Components/States/Systems).

## Testing Guidelines
- No dedicated test suite is present. Validate changes by building the APK and running on a target device.
- If you add tests, document how to run them here and keep test names aligned with module names.

## Commit & Pull Request Guidelines
- Commits use short, imperative subjects (e.g., “Fix Geometry”, “Update decoding and rendering”).
- PRs should include a clear description, build/test results, and screenshots or device notes for visual/VR changes.
- Link related issues or tasks when available.

## Configuration Tips
- The Android build expects a configured SDK/NDK and may use `local.properties` under `tridigenerator_player/Projects/Android/`.
- Native build settings are centralized in `CMakeLists.txt` and wired into Gradle via `externalNativeBuild`.
