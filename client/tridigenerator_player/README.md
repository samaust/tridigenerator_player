# tridigenerator_player

## Overview

This folder contains the native client code for the tridigenerator_player.
Below are quick reference tables that list the available Components, States
and Systems implemented under `Src/` to help navigation and documentation.

**Components**

- Directory: `client/tridigenerator_player/Src/Components`

| Component Header | Description |
|---|---|
| `FrameLoaderComponent.h` | Configuration and control for video/frame loading and playback. |
| `InputComponent.h` | Holds user input state (controller/touch/keyboard mappings). |
| `RenderComponent.h` | Generic rendering configuration for an entity (blend, materials). |
| `SceneComponent.h` | Scene-level metadata attached to an entity (scene grouping). |
| `TransformComponent.h` | Position, rotation and scale (pose) for an entity. |
| `UnlitGeometryRenderComponent.h` | Parameters specific to unlit YUV/alpha/depth geometry rendering. |

**States**

- Directory: `client/tridigenerator_player/Src/States`

| State Header | Description |
|---|---|
| `FrameLoaderState.h` | Runtime state for the frame loader (ring buffer, threads, indices). |
| `TransformState.h` | Cached matrices or derived transform state for rendering. |
| `UnlitGeometryRenderState.h` | GPU resources and texture sets for unlit geometry rendering. |

**Systems**

- Directory: `client/tridigenerator_player/Src/Systems`

| System | Source | Description |
|---|---:|---|
| `AudioSystem` | `AudioSystem.cpp` / `AudioSystem.h` | Audio playback and management. |
| `FrameLoaderSystem` | `FrameLoaderSystem.cpp` / `FrameLoaderSystem.h` | Downloads, demuxes and decodes video frames in a background thread. |
| `InputSystem` | `InputSystem.cpp` / `InputSystem.h` | Polls and processes user input events into components. |
| `RenderSystem` | `RenderSystem.cpp` / `RenderSystem.h` | High-level render queue submission and frame composition. |
| `SceneSystem` | `SceneSystem.cpp` / `SceneSystem.h` | Scene graph updates and entity lifecycle management. |
| `TransformSystem` | `TransformSystem.cpp` / `TransformSystem.h` | Applies and updates entity transforms and pose hierarchies. |
| `UnlitGeometryRenderSystem` | `UnlitGeometryRenderSystem.cpp` / `UnlitGeometryRenderSystem.h` | Renders unlit geometry using YUV + alpha + depth textures with double-buffering. |
| 
| ```

**Documentation**

The project includes a Doxygen configuration file located at `client/tridigenerator_player/Doxyfile`.
To (re)generate the HTML documentation using that file, run the following from the repository root:

```bash
cd client/tridigenerator_player
doxygen Doxyfile
```

The output directory is configured in `Doxyfile` (`Docs/html/`), so open the generated
`index.html` in a browser to view the documentation.

## Android Quest 3 build

The Android target is an `arm64-v8a` OpenXR application for Quest 3. This workflow builds a
debug APK for local sideloading; it does not produce a signed Meta Quest Store release.

### Prerequisites

Enable developer mode for the headset, connect it over USB, and accept the USB debugging prompt
inside the headset. Install Java 17 and Android SDK tools providing `adb`, Android platform API
32, an Android NDK, and CMake. Set the SDK location in
`Projects/Android/local.properties` without committing your machine-specific path:

```properties
sdk.dir=/absolute/path/to/Android/Sdk
```

### Build

Build the APK from the Android project directory:

```bash
cd client/tridigenerator_player/Projects/Android
./gradlew assembleDebug
```

On Windows, run `gradlew.bat assembleDebug` instead. The generated APK is
`build/outputs/apk/debug/tridigenerator_player-debug.apk`.

### Install and launch

Verify the connection, install the APK, and launch it with:

```bash
adb devices
adb install -r build/outputs/apk/debug/tridigenerator_player-debug.apk
adb shell am start -n io.github.samaust.tridigenerator_player/.MainActivity
```

### Android permissions

The application declares these permissions in `Projects/Android/AndroidManifest.xml`:

| Permission | Required | Use |
|---|---|---|
| `com.oculus.permission.USE_SCENE` | Yes | Provides scene access needed by the environment-depth integration. `MainActivity` requests it at startup and closes the application if it is denied. |
| `horizonos.permission.HEADSET_CAMERA` | No | Provides headset RGB camera frames for camera-based color and exposure matching. `MainActivity` requests it at startup; if it is denied, playback continues with color matching disabled. |
| `com.oculus.permission.HAND_TRACKING` | No | Enables tracked-hand poses, rendering, aim rays, and pinch selection in the dataset picker. Touch controllers remain available when hand tracking is unavailable. |
| `android.permission.INTERNET` | Yes | Downloads `/catalog.json`, ViPE manifests, and video data from the configured HTTP server. This is a normal install-time Android permission and does not display a runtime prompt. |

Passthrough is declared as a required device feature rather than an Android permission. The
manifest also requires VR head tracking and OpenGL ES 3.1, declares hand tracking as optional,
and enables cleartext HTTP traffic for development servers. Scene and headset-camera access are
the permissions explicitly requested by `MainActivity`; accept scene access to start the native
player, and grant headset-camera access if color matching is wanted.

### ViPE data server

The player loads ViPE data over HTTP; see [Android ViPE catalog](#android-vipe-catalog) for the
required catalog and manifest layout. The server base URL is currently set in
`Src/Systems/FrameLoaderSystem.cpp`, so update it to an address reachable from the headset and
rebuild the APK when the server address changes.

### Android ViPE catalog

Android retains HTTP loading. The configured server must expose `/catalog.json`:

```json
{
  "schema_version": 1,
  "datasets": [
    {
      "id": "dog-example",
      "display_name": "Dog Example",
      "manifest": "/vipe_encoded/dog-example.json"
    }
  ]
}
```

Each manifest's `file` is resolved relative to its manifest URL. The in-world picker uses
controller aim rays and trigger clicks; hand tracking is not required.

An optional `orientation_offset_degrees` manifest object corrects the reconstruction's
initial alignment. `yaw` rotates around OpenGL +Y, `pitch` around +X, and `roll` around +Z;
the world-space composition order is yaw, pitch, then roll. For example:

```json
"orientation_offset_degrees": {"yaw": 0.0, "pitch": 0.0, "roll": 0.0}
```

### Input controls

The in-world panel is the dataset picker. With Touch controllers, point either controller's aim
ray at a dataset button and squeeze its index trigger to select it. The trigger becomes active
after it passes the application's halfway threshold. With hand tracking, point either hand at a
button and pinch the index finger and thumb to select it. Either hand can interact; an actively
tracked hand takes precedence over the controller on the same side. Hand tracking is optional,
and the picker remains fully usable with Touch controllers. The Android implementation does not
currently assign locomotion, playback, mask, grip, face-button, or thumbstick actions.

### Troubleshooting

If `adb devices` reports `unauthorized`, reconnect the cable and accept the debugging prompt in
the headset. Gradle errors about missing SDK, NDK, CMake, or API 32 components indicate that the
corresponding package must be installed with the Android SDK Manager. If the picker reports that
it cannot load the catalog, confirm that the configured host and port are reachable from the
Quest and that the server exposes `/catalog.json`; `adb logcat` provides additional diagnostics.

## Linux desktop build

The Linux target is a native C++ X11/GLX application. It uses local ViPE data and does not
require an OpenXR runtime or network connection in its default desktop mode.

### Prerequisites

Install development packages providing CMake, pkg-config, X11, OpenGL, OpenXR Loader,
FFmpeg (`libavformat`, `libavcodec`, `libavutil`, `libswscale`), dav1d, and jsoncpp. Then:

```bash
sudo dnf install cmake gcc-c++ pkgconf-pkg-config openxr-devel ffmpeg-free-devel \
  libdav1d-devel jsoncpp-devel libX11-devel mesa-libGL-devel
```

### Build and test

```bash
cmake -S client -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux -j
ctest --test-dir build/linux --output-on-failure
```

### Desktop mode

Run the player against a local ViPE data directory and sequence:

```bash
./build/linux/tridigenerator_player/tridigenerator_player \
  --data-dir ./vipe_encoded --sequence dog-example --backend desktop
```

### OpenXR mode

`--backend openxr` uses the active system OpenXR runtime and requires a runtime that advertises
`XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`. [Monado](https://monado.freedesktop.org/) is
supported through the standard OpenXR loader: install Monado using your distribution packages
or build it separately, then select its runtime JSON using your system's OpenXR configuration
(or `XR_RUNTIME_JSON`). The player prints the selected runtime name at startup and remains
compatible with other conformant runtimes.

### Stereo mirror layouts

The X11 mirror can pack the tracked left and right eye views horizontally or vertically:

```bash
./build/linux/tridigenerator_player/tridigenerator_player \
  --data-dir ./vipe_encoded --sequence dog-example --backend openxr \
  --stereo-layout side-by-side

./build/linux/tridigenerator_player/tridigenerator_player \
  --data-dir ./vipe_encoded --sequence dog-example --backend openxr \
  --stereo-layout over-under
```

Side by side is the default.

### Input controls

In desktop mode, controls are mouse-look, `W/A/S/D`, `Q/E` for vertical movement, and Shift for
faster movement. `C` restores the camera's starting position and orientation. Escape releases
the pointer; press Escape again to exit. Left click recaptures it.

In OpenXR mode the runtime controls head orientation and eye poses; `W/A/S/D` and `Q/E` add a
locomotion offset, Shift increases movement speed, and `C` clears the offset. Mouse-look remains
exclusive to desktop mode. In both modes, Space pauses or resumes video playback without
disabling camera movement, and `M` toggles mask filtering.
