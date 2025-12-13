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
