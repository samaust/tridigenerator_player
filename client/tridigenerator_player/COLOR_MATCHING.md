# Android color matching implementation

The Android player uses the Quest headset RGB camera to make decoded ViPE geometry match the
color and exposure of the passthrough scene. The correction is computed in linear RGB and applied
only to the rendered geometry; it does not modify the source video or the passthrough layer.

Color matching has two progressively stricter tiers:

| Tier | Estimate | Requirements |
|---|---|---|
| `Unavailable` | No active correction. The last correction fades out. | Used when matching is disabled, the app is unfocused, camera data is absent or stale, or an estimate has expired. |
| `Global` | One tint and exposure value for the complete object. | A fresh, valid headset RGB camera frame. |
| `Spatial` | A trilinearly sampled `16 x 12 x 16` tint/exposure light field around the object. | Everything required by Global, plus environment depth, calibrated camera pose, OpenXR timestamp conversion, and a valid object transform. |

Missing camera permission or unavailable XR features do not prevent playback. The renderer falls
back from Spatial to Global, or fades matching out when no estimate is available.

The Android UI exposes these as the requested tiers `Disabled`, `Global`, and `Spatial`. The
requested tier is a quality cap, while `LightEstimateTier` reports what the estimator is currently
achieving. Spatial may therefore be selected while the active result temporarily reads Global.

## Pipeline

```text
MainActivity permission request
        |
        v
Camera2 NDK left passthrough RGB camera
        |
        v
AImageReader YUV_420_888 callback
        |
        +-------------------------------+
        |                               |
        v                               v
CPU global estimate             Environment depth + camera pose
(tint and exposure)                     |
        |                               v
        |                       ES 3.1 compute shader
        |                       16 x 12 x 16 light field
        |                               |
        +---------------+---------------+
                        v
             Unlit geometry fragment shader
                        |
                        v
        linear video RGB * tint * exposure
```

The application update order is important. [`TDGenPlayerApp::Update()`](Src/Core/TDGenPlayerApp.cpp)
updates the object transform, acquires environment depth, updates the light estimate, and finally
updates the geometry renderer. This lets color estimation consume the current depth and transform
state before the render uniforms are prepared.

## Android and OpenXR prerequisites

[`AndroidManifest.xml`](Projects/Android/AndroidManifest.xml) declares OpenGL ES 3.1 and the two
permissions relevant to matching:

- `horizonos.permission.HEADSET_CAMERA` provides RGB camera frames. It is optional for playback.
- `com.oculus.permission.USE_SCENE` provides scene access needed by environment depth. The current
  launcher treats it as required for starting the native activity.

[`MainActivity`](java/MainActivity.java) requests both permissions before launching
`MainNativeActivity`. If headset-camera access is denied, it logs that matching is disabled and
continues. Native code does not receive a separate permission flag: camera startup simply fails
when the camera cannot be opened.

The native Android target links `camera2ndk`, `mediandk`, and `GLESv3` in
[`CMakeLists.txt`](CMakeLists.txt). Camera-specific code in
[`CameraLightEstimationSystem.cpp`](Src/Systems/CameraLightEstimationSystem.cpp) is guarded by
`#if defined(__ANDROID__)`.

The app requests these OpenXR extensions in [`TDGenPlayerApp::GetExtensions()`](Src/Core/TDGenPlayerApp.cpp):

- `XR_META_environment_depth` supplies spatial samples and depth-camera projection data.
- `XR_KHR_convert_timespec_time` maps Android camera timestamps to `XrTime`.
- The passthrough extensions support the surrounding mixed-reality presentation, but they are not
  themselves inputs to the color estimator.

[`CoreSystem::Init()`](Src/Systems/CoreSystem.cpp) records whether timestamp conversion is available,
and `CoreSystem::InitPassthrough()` loads the environment-depth entry points. The
[`EnvironmentDepthSystem`](Src/Systems/EnvironmentDepthSystem.cpp) creates and starts the depth
provider, owns its swapchain, and sets `EnvironmentDepthState::HasDepth` only after a successful
acquisition for the current frame.

## Components, state, and lifecycle

[`CameraLightEstimationComponent`](Src/Components/CameraLightEstimationComponent.h) contains the
configuration:

| Field | Default | Purpose |
|---|---:|---|
| `requestedTier` | `Spatial` | Selects Disabled, Global, or Spatial as the maximum estimation tier. |
| `diagnosticOverlay` | `false` | Reserved; no overlay currently consumes this field. |
| `matchingStrength` | `1.0` | Multiplies the final fade amount in the render shader. |
| `updateRateHz` | `10.0` | Maximum spatial compute dispatch rate. Global estimation is not rate-limited by this value. |
| `temporalSmoothing` | `0.85` | Fraction of the previous estimate retained during smoothing. |
| `minExposure` / `maxExposure` | `0.35` / `2.0` | Bounds for the exposure multiplier. |
| `minTint` / `maxTint` | `0.7` / `1.4` | Per-channel tint bounds. |
| `maximumFrameAgeSeconds` | `0.25` | Maximum accepted camera-frame age. |
| `estimateHoldSeconds` | `1.0` | Time without a successful estimate before the tier becomes Unavailable. |

[`CameraLightEstimationState`](Src/States/CameraLightEstimationState.h) owns the runtime results and
GPU resources:

- Three `GL_R8` camera-plane textures for Y, U, and V.
- The compute program and `GL_RGBA16F` 3D light-field texture.
- `globalLight`, stored as tint RGB plus exposure in alpha.
- Camera intrinsics, image dimensions, calibration status, and camera/local transforms.
- The current `LightEstimateTier`, its transition blend, and update timestamps.
- A default four-by-three-by-four metre grid, recentered on the rendered object for spatial updates.

`CameraLightEstimationPlatformState`, defined privately in
[`CameraLightEstimationSystem.cpp`](Src/Systems/CameraLightEstimationSystem.cpp), owns the Camera2 NDK
objects, latest CPU camera frame, calibration metadata, and capture flags.

The system lifecycle is wired in [`TDGenPlayerApp`](Src/Core/TDGenPlayerApp.cpp):

1. `Init()` creates the platform state.
2. `SessionInit()` permits a new camera-start attempt.
3. `Update()` starts or stops capture according to focus, consumes frames, and calculates estimates.
4. `SessionEnd()` stops the Android camera.
5. `Shutdown()` also deletes camera textures, the 3D texture, and the compute program.

Losing XR focus stops the camera and clears the one-attempt guard. Regaining focus permits a new
start attempt. Within one continuously focused session, a failed start is not retried.

Selecting Disabled also stops capture, but retains the last known camera capability so the UI can
offer Global and Spatial again. Selecting Global restarts capture but skips spatial dispatches.

## Camera selection and frame capture

`StartCamera()` enumerates Camera2 devices and uses Meta vendor metadata to select the left RGB
passthrough camera:

```text
META_CAMERA_SOURCE_TAG   == 0  (RGB)
META_CAMERA_POSITION_TAG == 0  (left)
```

The camera is accepted only when all of these calibration conditions are met:

- Lens pose is referenced to the gyroscope.
- Sensor timestamps use the realtime time base.
- A pre-correction active-array rectangle is present.
- Intrinsic calibration, lens rotation, and lens translation are present.
- A non-input `YUV_420_888` stream between 640 and 1280 pixels wide is available.

The smallest qualifying stream is selected. Lens distortion coefficients are read when present;
missing coefficients remain zero and do not invalidate calibration. Intrinsics are adjusted from
the sensor active array to the centered crop used by the selected output resolution.

An `AImageReader` with a two-image queue receives `YUV_420_888` frames. `OnImageAvailable()` acquires
the latest image, records its sensor timestamp, and copies all three planes into tightly packed CPU
buffers. `CopyPlane()` honors each plane's row stride and pixel stride. A mutex protects publication
of the latest frame, and a sequence number prevents the render thread from consuming it twice.

During `Update()`, a frame is rejected when it is not new, has an empty plane, has a future or
non-positive timestamp, or is older than `maximumFrameAgeSeconds`. Accepted planes are uploaded to
single-channel textures with linear filtering and clamp-to-edge wrapping. Texture storage is
reallocated only when a plane's dimensions change.

## Global estimation

Global estimation runs on the CPU for every accepted camera frame. It does not require environment
depth or OpenXR time conversion.

The estimator samples pixels at 16-pixel intervals, starting at `(8, 8)`. Y is read at full
resolution and U/V at half resolution. [`CameraLightMath::YuvToLinear()`](Src/Systems/CameraLightMath.h)
interprets the camera as limited-range YUV and converts it to linear RGB:

```text
C = Y - 0.0625
D = U - 0.5
E = V - 0.5

R' = 1.1643 C + 1.5958 E
G' = 1.1643 C - 0.39173 D - 0.81290 E
B' = 1.1643 C + 2.0170 D
```

Each nonlinear channel is clamped to `[0, 1]` and decoded with the standard sRGB transfer
function. Linear luminance is then:

```text
L = 0.2126 R + 0.7152 G + 0.0722 B
```

Samples with `L <= 0.005` or `L >= 0.98` are discarded to reduce the influence of black and
saturated regions. Valid luminances are accumulated in log space. `TrimmedMean()` removes the
lowest and highest 10 percent before the mean is exponentiated, producing a robust geometric mean.

The target exposure maps that mean to 18-percent gray:

```text
exposure = clamp(0.18 / max(L, 0.001), minExposure, maxExposure)
```

Tint is the arithmetic mean RGB normalized by its average channel:

```text
channelAverage = max((R + G + B) / 3, 0.001)
tint = clamp(meanRGB / channelAverage, minTint, maxTint)
```

The target is stored as `(tint.r, tint.g, tint.b, exposure)` and smoothed on the CPU:

```text
globalLight = previous * temporalSmoothing
            + target   * (1 - temporalSmoothing)
```

A valid result sets the tier to Global and refreshes the estimate timestamp. The tint follows the
observed environmental color cast; this is ambient color matching, not a neutralizing white-balance
operation.

## Spatial estimation and compute shader

Spatial estimation is attempted after the Global result is available. In addition to an accepted
camera frame, it requires:

- `CoreComponent::supportsTimeConversion` and a loaded conversion function.
- A valid OpenXR view space.
- `EnvironmentDepthState::HasDepth` for the current update.
- A valid object `TransformState`.
- At least `1 / updateRateHz` seconds since the previous spatial dispatch.

The camera sensor timestamp is converted to `XrTime`, then `xrLocateSpace()` obtains the headset
view pose at capture time. Both position and orientation flags must be valid. The calibrated lens
pose is composed with that headset pose and an Android-camera-to-OpenXR transform that flips Y and
Z. This aligns the camera image with depth-derived local points at the time of capture rather than
at the current render pose.

The spatial grid is centered on the rendered object's current model translation. On first use, the
system builds the embedded OpenGL ES 3.1 compute shader in `BuildComputeProgram()` and allocates a
trilinearly filtered `16 x 12 x 16` `GL_RGBA16F` 3D texture.

The shader declares `local_size_x/y/z = 4`; `glDispatchCompute(4, 3, 4)` therefore launches exactly
one invocation for every voxel. Each invocation:

1. Computes the voxel center within the object-centered grid.
2. Samples layer zero of environment depth over a fixed `32 x 24` grid.
3. Rejects depth samples outside `(0, 1)` and unprojects valid samples into local space.
4. Transforms each point to the captured RGB camera coordinate system.
5. Rejects points behind or within 5 cm of the camera.
6. Applies the five stored radial-distortion coefficients and camera intrinsics.
7. Rejects projections outside the RGB image and samples the Y/U/V textures.
8. Converts limited-range camera YUV to linear RGB and rejects luminance outside `(0.005, 0.98)`.
9. Weights the RGB sample by its squared distance from the voxel:

   ```text
   weight = exp(-distanceSquared / 1.125)  (sigma = 0.75 m)
   ```

If total weight is below `0.05`, the voxel receives `globalLight`. Otherwise the shader derives tint
and exposure from the weighted mean, using the same bounds as Global. Unlike the CPU estimator, the
spatial exposure uses ordinary weighted mean luminance rather than a trimmed geometric mean.

After the first dispatch, each new voxel estimate is mixed with its previous value using
`temporalSmoothing`. A texture-fetch and shader-image memory barrier makes writes visible to the
fragment shader. Successful dispatch changes the tier to Spatial.

## Render shader application

[`UnlitGeometryRenderSystem`](Src/Systems/UnlitGeometryRenderSystem.cpp) packs matching state into
`u_lightParams`:

| Matrix row | Values |
|---|---|
| 0 | Global tint RGB and global exposure |
| 1 | Grid minimum XYZ and numeric tier |
| 2 | Inverse grid extent XYZ and `tierBlend` |
| 3 | `matchingStrength` and unused values |

When available, the light-field texture is bound through `TEX_LIGHT_FIELD`, declared in
[`UnlitGeometryRenderComponent.h`](Src/Components/UnlitGeometryRenderComponent.h).

The final correction is implemented by the unlit geometry fragment shader in
[`UnlitGeometryRenderShaders.h`](Src/Shaders/UnlitGeometryRenderShaders.h). The shader first selects
the appropriate limited- or full-range conversion for the decoded ViPE video and converts that
video color to linear RGB. This selection is independent of camera conversion, which is always
limited-range.

The Global estimate is the default. When the tier is Spatial and a fragment's world position falls
inside the light-field bounds, the shader samples the 3D texture; fragments outside the grid retain
the Global estimate. It then applies:

```text
matchAmount = clamp(tierBlend * matchingStrength, 0, 1)
correctedRGB = videoRGB
             * mix(1, estimatedTint * estimatedExposure, matchAmount)
```

This multiplication occurs in linear RGB before environment-depth occlusion is evaluated. The
occlusion path may subsequently alter fragment alpha and depth, but not its matched color.

## Tier transitions and degradation

### Android controls

The dataset and mask screens each provide a `Color matching` button. It opens a dedicated screen
that can be operated with the existing Touch-controller aim and index trigger or hand aim and index
pinch. The screen shows the selected tier, active tier, and one row for Disabled, Global, and
Spatial. The selected tier and unavailable tiers are labels rather than hit-testable buttons.

Disabled is always selectable. Global remains in `Checking` until calibrated camera startup either
succeeds or conclusively fails. Spatial is selectable only when Global capability, timestamp
conversion, initialized environment depth, view space, and an object transform are available.
Temporary stale frames or individual depth-acquisition failures change the active result but do
not remove an otherwise supported tier from the UI.

When a selected capability conclusively becomes unavailable, selection automatically downgrades to
the highest lower available tier: Spatial becomes Global when spatial prerequisites are unsupported,
and either camera-backed tier becomes Disabled when camera startup or capture fails. The initial
selection is Spatial and is not persisted across application launches.

`lastEstimateSeconds` is refreshed by either a successful Global calculation or a Spatial dispatch.
If it becomes older than `estimateHoldSeconds`, the tier changes to Unavailable. `tierBlend` moves
toward one while either estimate tier is available and toward zero while unavailable, taking
approximately 0.5 seconds for a complete transition. This avoids abrupt color changes when camera
frames are briefly interrupted.

The renderer behaves as follows:

- Spatial data missing, but camera valid: use Global matching.
- Fragment outside the spatial grid: use Global matching.
- Spatial voxel has insufficient support: the compute shader stores the Global estimate there.
- Estimate missing or expired: fade toward the unmodified video color.
- `matchingStrength == 0`: continue estimation but apply no visible correction.
- Camera permission denied or camera unavailable: continue playback without matching.

Tier changes are logged by `CameraLightEstimationSystem` as `unavailable`, `global`, or `spatial`.
Camera selection, capture startup, calibration failure, shader compilation/linking, disconnection,
and camera errors also emit native logs under the `CameraLightEstimation` tag.

## Tests

[`CameraLightMathTests.cpp`](Tests/CameraLightMathTests.cpp) covers the platform-independent helper
functions:

- Limited-range YUV black conversion.
- Full-range neutral-gray conversion.
- Voxel indexing boundaries.
- Gain clamping.
- Trimmed-mean behavior.
- Fresh, stale, and future camera timestamps.

The test is registered as `camera_light_math_tests` by the non-Android CMake test configuration.
Camera selection, OpenXR alignment, compute dispatch, and final visual matching currently require
on-device validation.

## Known limitations

- `diagnosticOverlay` is declared but not implemented.
- The UI selects the tier but does not expose numeric matching parameters.
- Headset camera YUV is always interpreted as limited range.
- Global estimation runs for every accepted frame; `updateRateHz` limits only Spatial dispatches.
- The compute shader samples environment-depth array layer zero rather than combining both views.
- Spatial sampling uses a fixed `32 x 24` depth grid and a fixed `16 x 12 x 16` light-field size.
- A failed camera start is not retried until focus or session state resets the start guard.
- Lens distortion metadata is optional; absent coefficients silently behave as zero distortion.
- Spatial and Global exposure use different averaging strategies.
- Automated tests cover math helpers only, not camera metadata, GPU shaders, or on-device output.
