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

**Documentation**

See [Color matching implementation](COLOR_MATCHING.md) for the Android headset-camera color and
exposure pipeline, including its Global and Spatial estimation tiers and GPU shaders.

The project includes a Doxygen configuration file located at `client/tridigenerator_player/Doxyfile`.
To (re)generate the HTML documentation using that file, run the following from the repository root:

```bash
cd client/tridigenerator_player
doxygen Doxyfile
```

The output directory is configured in `Doxyfile` (`Docs/html/`), so open the generated
`index.html` in a browser to view the documentation.

## ViPE encoded data format

A ViPE encoded sequence consists of a JSON manifest and the Matroska (`.mkv`) file
named by its `file` field. Keep both files together: the player resolves `file` relative to the
manifest location. Android downloads them over HTTP, while Linux reads them from the local data
directory.

### Matroska stream contract

The Matroska file contains exactly three synchronized video streams:

| Index | Content | Codec | Pixel format | Value |
|---:|---|---|---|---|
| 0 | Color | AV1 | `yuv420p` | 8-bit YUV 4:2:0 color. |
| 1 | Mask | FFV1 | `gray` | 8-bit label or visibility value. |
| 2 | Depth | PNG | `gray16be` | Big-endian unsigned 16-bit linear depth. |

All three streams must use the manifest's `width`, `height`, and rational `frame_rate`, and each
must contain `frame_count` frames. For each output frame, the player decodes one color, mask, and
depth frame as a synchronized set. Audio and additional video streams are not part of the
stream contract.

Inspect an encoded file with:

```bash
ffprobe -v error -count_frames \
  -show_entries stream=index,codec_name,pix_fmt,width,height,avg_frame_rate,nb_read_frames \
  -of compact=p=0:nk=0 vipe_encoded/dog-example.mkv
```

### JSON manifest

This single-frame example shows the complete structure accepted by the player:

```json
{
  "schema_version": 2,
  "file": "example.mkv",
  "sequence": "example",
  "frame_count": 1,
  "width": 1280,
  "height": 720,
  "frame_rate": {
    "numerator": 2997,
    "denominator": 100
  },
  "orientation_offset_degrees": {
    "yaw": 0.0,
    "pitch": 0.0,
    "roll": 0.0
  },
  "streams": {
    "color": {"index": 0, "codec": "av1", "pixel_format": "yuv420p"},
    "mask": {"index": 1, "codec": "ffv1", "pixel_format": "gray"},
    "depth": {"index": 2, "codec": "png", "pixel_format": "gray16be"}
  },
  "depth": {
    "encoding": "uint16_linear",
    "units": "metres",
    "units_per_metre": 8507.586206896553,
    "invalid_value": 0
  },
  "pose": {
    "type": "camera_to_world",
    "matrix_layout": "row_major",
    "coordinate_convention": "opencv_x_right_y_down_z_forward",
    "frame_indices": [0],
    "matrices": [[
      1.0, 0.0, 0.0, 0.0,
      0.0, 1.0, 0.0, 0.0,
      0.0, 0.0, 1.0, 0.0,
      0.0, 0.0, 0.0, 1.0
    ]]
  },
  "intrinsics": {
    "frame_indices": [0],
    "camera_models": ["PINHOLE"],
    "values": [[989.2, 989.3, 640.0, 360.0]]
  },
  "mask_labels": {
    "0": "background",
    "2": "animal"
  },
  "color_reference": {
    "color_space": "linear_srgb",
    "aggregation": "sequence",
    "global": {
      "chromaticity": [1.0, 1.0, 1.0],
      "log_average_luminance": 0.18,
      "sample_count": 921600
    },
    "masks": {
      "2": {
        "chromaticity": [1.08, 0.99, 0.81],
        "log_average_luminance": 0.21,
        "sample_count": 204800
      }
    }
  }
}
```

The required identity and timing fields are `schema_version`, `file`, `sequence`, `frame_count`,
`width`, `height`, and `frame_rate`. Schema versions 1 and 2 are supported. Dimensions and frame
count must be positive, and both frame-rate values must be positive integers. The `streams`
object must match the three fixed entries in the stream table. The `depth` object must specify
`uint16_linear` encoding, `metres`, a positive finite `units_per_metre`, and a uint16
`invalid_value`.

Depth samples convert to metric distance as:

```text
depth_metres = encoded_value / units_per_metre
```

An encoded value equal to `invalid_value` (normally zero) has no valid depth. The required
`pose` and `intrinsics` arrays must each have exactly `frame_count` entries, with contiguous,
zero-based `frame_indices`. Every pose is a finite, rigid, row-major 4x4 camera-to-world matrix
using OpenCV axes (+X right, +Y down, +Z forward). Each camera model must be `PINHOLE`, with
intrinsics ordered `[fx, fy, cx, cy]` in pixels and positive focal lengths.

`orientation_offset_degrees` is optional and defaults to zero rotation. It corrects a
reconstruction's initial alignment: yaw rotates around OpenGL +Y, pitch around +X, and roll
around +Z, with world-space composition order yaw, pitch, then roll. `mask_labels` is also
optional; it maps uint8 keys written as JSON strings (`"0"` through `"255"`) to human-readable
label names.

Schema version 2 requires `color_reference`, generated by preprocessing from valid rendered pixels
across the sequence. It contains a global linear-sRGB chromaticity/luminance reference and reliable
per-mask references. Missing per-mask entries fall back to the global reference. Schema-v1 datasets
remain playable, but Android color matching is unavailable for them.

The encoder also emits descriptive fields that the current player does not validate or consume:
top-level `fps`; `depth_scale_factor`, `max_depth_metres`, `valid_source_pixels`, and
`invalid_source_pixels` inside `depth`; and `layout` and `units` inside `intrinsics`. These values
are useful for inspection, but the required fields above define playback behavior.

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

### On-device validation

After launching the player on a Quest 3, verify the interaction paths before relying on them in
a demo or test session:

1. Point each Touch controller at the mesh, hold its grip control, and confirm that controller
   translation and rotation move and rotate the mesh without snapping.
2. While either controller is holding the mesh, move the right thumbstick up and down and confirm
   that the mesh moves farther away and closer, respectively.
3. Grab the mesh with both controllers, move the controllers together and apart, and confirm that
   the mesh translates, rotates, and scales uniformly without exceeding `0.01x` or `100x`.
4. Repeat the single- and two-hand interactions using index-finger pinches after switching to hand
   tracking.
5. Release one actor during a two-actor grab and confirm that the remaining actor continues with a
   stable single grab. Hide or untrack an active actor and confirm that its grab is released.
6. Press X on the left controller, then clap both tracked hands, and confirm that each action hides
   or restores the current UI without resetting its screen or selections.
7. Confirm that grab, release, two-controller, scale-limit, and X-button UI events vibrate the
   expected left or right controller.
8. Open `Color matching` from both the dataset and mask screens. Select Disabled, Global, and
   Spatial with a controller trigger and a hand pinch; confirm the status shows both the selected
   and currently active tier and that unavailable tiers cannot be selected.
9. Select the centered `⏸` control in the bottom playback bar with a controller trigger and a hand
   pinch. Confirm color, mask, depth, pose, and intrinsics remain frozen until selecting `▶`.
10. Hide the UI and press A on the right controller to pause and resume. Restore the UI and confirm
    the icon matches playback state. Pause again, select another dataset, and confirm it resumes.

### Android permissions

The application declares these permissions in `Projects/Android/AndroidManifest.xml`:

| Permission | Required | Use |
|---|---|---|
| `com.oculus.permission.USE_SCENE` | Yes | Provides scene access needed by the environment-depth integration. `MainActivity` requests it at startup and closes the application if it is denied. |
| `horizonos.permission.HEADSET_CAMERA` | No | Provides headset RGB camera frames for camera-based color and exposure matching. `MainActivity` requests it at startup; if it is denied, playback continues with color matching disabled. |
| `com.oculus.permission.HAND_TRACKING` | No | Enables tracked-hand poses, rendering, aim rays, and pinch selection in the dataset and mask panels. Touch controllers remain available when hand tracking is unavailable. |
| `android.permission.INTERNET` | Yes | Downloads `/catalog.json`, ViPE manifests, and video data from the configured HTTP server. This is a normal install-time Android permission and does not display a runtime prompt. |

Passthrough is declared as a required device feature rather than an Android permission. The
manifest also requires VR head tracking and OpenGL ES 3.1, declares hand tracking as optional,
and enables cleartext HTTP traffic for development servers. Scene and headset-camera access are
the permissions explicitly requested by `MainActivity`; accept scene access to start the native
player, and grant headset-camera access if color matching is wanted.

For the permission, camera-capture, environment-depth, estimation, and shader data flow, see
[Color matching implementation](COLOR_MATCHING.md).

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

Each manifest's `file` is resolved relative to its manifest URL. See
[ViPE encoded data format](#vipe-encoded-data-format) for the Matroska stream contract and
manifest fields. New preprocessing output uses schema v2; legacy schema-v1 manifests play without
color matching.

### Input controls

The in-world panel initially shows the masks for the automatically loaded dataset. Every uint8
mask ID, including background ID 0 and IDs not listed in the manifest, starts visible. Each
manifest-labeled mask has a `Visible`/`Hidden` toggle; selecting another dataset resets all masks
to visible. Use `Back to datasets` to replace the mask panel with the dataset picker. The current
video and mask choices continue rendering while the picker is open, until another dataset is
selected.

The Android build assigns these controller and hand-tracking controls:

| Action | Touch controllers | Hand tracking |
|---|---|---|
| Select a UI control | Aim at it and squeeze the index trigger past the halfway threshold. | Aim at it and pinch the index finger and thumb. |
| Start and hold a mesh grab | Aim at the mesh and hold the grip control. | Aim at the mesh and hold an index-finger pinch. |
| Move and rotate the mesh | Translate or rotate the controller while holding the grab. | Move or rotate the tracked hand while holding the pinch. |
| Adjust grab distance | Move the right thumbstick up to move farther away or down to move closer. This works regardless of which controller started the grab. | No direct hand-only distance control; the right controller thumbstick remains usable when available. |
| Two-actor transform | Aim both controller rays at the mesh and hold both grips, then move the controllers to translate, rotate, and uniformly scale it. | Aim both hand rays at the mesh and hold both pinches, then move the hands to translate, rotate, and uniformly scale it. |
| Toggle UI visibility | Press X on the left controller. | Bring both valid, mutually facing palms together in a deliberate clap. |
| Pause or resume playback | Press A at any time, or select the centered `⏸`/`▶` control in the bottom bar while the UI is visible. | Aim at the bottom `⏸`/`▶` control and pinch while the UI is visible. |
| Select color-matching tier | Open `Color matching`, aim at an available Disabled, Global, or Spatial row, and press the index trigger. | Open `Color matching`, aim at an available tier, and pinch the index finger and thumb. |
| Edit color-matching settings | Select `Edit settings`, then use the `-`, value, and `+` controls. Select Save on the overview to persist the preview. | Use the same controls with an aim pinch; leaving the overview without Save restores the prior values. |

Uniform mesh scale is clamped to `0.01x` through `100x`, where `1x` is the original size. Both
rays must hit the mesh before a two-actor transform begins. Two controllers or two hands can be
used together, but a mixed controller-and-hand pair cannot enter two-actor mode. Releasing one
actor re-baselines the remaining actor as a stable single grab; releasing both, or losing the
required tracking pose, ends the corresponding grab.

An actively tracked hand takes precedence over the controller on the same side. Hand tracking is
optional, and the UI and mesh remain usable with Touch controllers when it is unavailable. Clap
detection is disabled while the mesh is being manipulated to prevent an interaction from
unexpectedly hiding the UI. Locomotion controls remain unassigned.

The wide playback bar appears at the bottom of every Android UI screen. It shows `⏸` while the
video is playing and `▶` while paused; the icon describes the action that selecting it performs.
Pausing freezes the synchronized color, mask, depth, pose, and intrinsics frame while the OpenXR
render loop and tracking continue. Pressing A works even when the UI and playback bar are hidden.
A successful dataset change always starts the newly selected video playing.

The color-matching screen is reachable from both the dataset and mask screens. Disabled is always
available; Global and Spatial are non-interactive while their camera or environment-depth
prerequisites are being checked or are unavailable. Spatial remains selected during transient depth
loss and reports Global as the active fallback, but a conclusively unsupported selected tier is
automatically downgraded.

The overview can edit strength, smoothing, tint bounds, and exposure bounds. Changes are previewed
live. `Save` stores the tier and numeric settings for the current catalog dataset ID in Android
private storage; switching datasets or restarting restores that dataset's values. `Reset defaults`
previews the canonical values and Spatial tier, but does not persist them until Save is selected.
Backing out of the overview discards unsaved changes. Runtime capability downgrades do not overwrite
the saved requested tier.

### Haptics

Touch controllers provide short vibration cues when a controller successfully grabs or releases
the mesh, when a second controller starts a two-controller transform, and when scaling first
reaches either limit. Pressing X to toggle the UI also vibrates the left controller. Two-controller
events are routed to the participating controllers rather than broadcast to every device.

Hand-only interactions and clap-based UI toggles do not produce haptic feedback. Haptic failures
are non-fatal: mesh manipulation and UI controls continue working if the runtime or active device
does not provide the requested vibration output.

### Troubleshooting

If `adb devices` reports `unauthorized`, reconnect the cable and accept the debugging prompt in
the headset. Gradle errors about missing SDK, NDK, CMake, or API 32 components indicate that the
corresponding package must be installed with the Android SDK Manager. If the picker reports that
it cannot load the catalog, confirm that the configured host and port are reachable from the
Quest and that the server exposes `/catalog.json`.

If a mesh grab does not begin, wait until a decoded mesh frame is visible, aim directly through
the visible mesh, and start a new grip or pinch; selection requires a fresh press whose ray hits
the current depth-derived bounds. If hands are not available, confirm that hand tracking is
enabled in the headset, the optional hand-tracking permission is granted, and both palms or aim
poses are visible to the headset cameras. If controller interaction works without vibration,
make sure the application is focused and test each Touch controller independently; unsupported
haptics do not block input. For clap toggles, first separate both hands, face the palms toward one
another, and bring them together deliberately while no mesh grab is active. Keep idle hands
separated if unintended clap toggles occur.

Use `adb logcat` to inspect native input, hand-tracking, OpenXR, haptic, and loading diagnostics
while reproducing an issue:

```bash
adb logcat | grep -E 'TDGenPlayerApp|InputSystem|OpenXR|FrameLoader'
```

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
disabling camera movement.

The mask visibility panel in the upper-left lists the manifest's mask IDs and labels. Every mask,
including background ID 0, is visible when the player starts. Click a checkbox to show or hide one
mask, use `Show all` or `Hide all` for bulk changes, and use the mouse wheel to scroll long lists.
The panel is visible at startup; press `M` to hide or show it without changing mask selections.
In desktop mode, press Escape to release the captured pointer before interacting with the panel;
click outside the panel to recapture mouse-look. Selections last for the current session only and,
in OpenXR mode, apply to both the headset view and the mirror.
