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

## Linux desktop build

The Linux target is a native C++ X11/GLX application. It uses local ViPE data and does not
require an OpenXR runtime or network connection in its default desktop mode.

Install development packages providing CMake, pkg-config, X11, OpenGL, OpenXR Loader,
FFmpeg (`libavformat`, `libavcodec`, `libavutil`, `libswscale`), dav1d, and jsoncpp. Then:

```bash
sudo dnf install cmake gcc-c++ pkgconf-pkg-config openxr-devel ffmpeg-free-devel \
  libdav1d-devel jsoncpp-devel libX11-devel mesa-libGL-devel
```

```bash
cmake -S client -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux -j
ctest --test-dir build/linux --output-on-failure
./build/linux/tridigenerator_player/tridigenerator_player \
  --data-dir ./vipe_encoded --sequence dog-example --backend desktop
```

Controls are mouse-look, `W/A/S/D`, `Q/E` for vertical movement, and Shift for faster
movement. `C` restores the camera's starting position and orientation. Space pauses or
resumes video playback without disabling camera movement. `M` toggles mask filtering. Escape
releases the pointer; press Escape again to exit. Left click recaptures it.

`--backend openxr` validates that the configured runtime advertises
`XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO` before starting the desktop mirror. Runtime
presentation remains dependent on a compatible user-supplied runtime/HMD setup.

## Android ViPE catalog

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
