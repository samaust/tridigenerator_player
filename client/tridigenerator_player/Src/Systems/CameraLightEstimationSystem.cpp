#include "CameraLightEstimationSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <GLES3/gl31.h>

#include "XrApp.h"
#include "../Components/CameraLightEstimationComponent.h"
#include "../Components/CoreComponent.h"
#include "../Components/TransformComponent.h"
#include "../Components/FrameLoaderComponent.h"
#define LOG_TAG "CameraLightEstimation"
#include "../Core/Logging.h"
#include "../States/CoreState.h"
#include "../States/EnvironmentDepthState.h"
#include "../States/TransformState.h"
#include "CameraLightMath.h"

#if defined(__ANDROID__)
#include <android/native_window.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;

double NowSeconds() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

int64_t NowNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count();
}

struct CameraFrame {
    int width = 0;
    int height = 0;
    int64_t timestampNs = 0;
    uint64_t sequence = 0;
    std::vector<uint8_t> planes[3];
};

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Light field shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint BuildComputeProgram() {
    static const char* source = R"glsl(#version 310 es
precision highp float;
precision highp int;
layout(local_size_x=4, local_size_y=4, local_size_z=4) in;
layout(rgba16f, binding=0) uniform highp image3D u_output;
uniform highp sampler2DArray u_depth;
uniform highp sampler2D u_y;
uniform highp sampler2D u_u;
uniform highp sampler2D u_v;
uniform highp mat4 u_depthToLocal;
uniform highp mat4 u_cameraFromLocal;
uniform highp vec4 u_intrinsics;
uniform highp vec4 u_distortion;
uniform highp float u_distortionK5;
uniform highp vec2 u_imageSize;
uniform highp vec3 u_gridMinimum;
uniform highp vec3 u_gridExtent;
uniform highp vec4 u_globalLight;
uniform highp float u_temporalSmoothing;
uniform lowp int u_hasPrevious;

vec3 srgbToLinear(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    bvec3 lo = lessThanEqual(c, vec3(0.04045));
    return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lo);
}
vec3 cameraRgb(vec2 uv) {
    float y = texture(u_y, uv).r;
    float uu = texture(u_u, uv).r - 0.5;
    float vv = texture(u_v, uv).r - 0.5;
    float c = y - 0.0625;
    return srgbToLinear(vec3(
        1.1643*c + 1.5958*vv,
        1.1643*c - 0.39173*uu - 0.81290*vv,
        1.1643*c + 2.017*uu));
}
void main() {
    ivec3 id = ivec3(gl_GlobalInvocationID);
    ivec3 size = imageSize(u_output);
    if (any(greaterThanEqual(id, size))) return;
    vec3 voxel = u_gridMinimum + (vec3(id) + 0.5) / vec3(size) * u_gridExtent;
    vec3 sum = vec3(0.0);
    float weightSum = 0.0;
    const int SX = 32;
    const int SY = 24;
    for (int yy=0; yy<SY; ++yy) {
        for (int xx=0; xx<SX; ++xx) {
            vec2 duv = (vec2(xx, yy) + 0.5) / vec2(SX, SY);
            float d = texture(u_depth, vec3(duv, 0.0)).r;
            if (d <= 0.0 || d >= 1.0) continue;
            vec4 local = u_depthToLocal * vec4(duv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
            if (abs(local.w) < 0.00001) continue;
            local /= local.w;
            vec4 camera = u_cameraFromLocal * local;
            float forward = -camera.z;
            if (forward <= 0.05) continue;
            vec2 normalized = vec2(camera.x / forward, -camera.y / forward);
            float r2 = dot(normalized, normalized);
            float radial = (1.0 + u_distortion.x*r2 + u_distortion.y*r2*r2 + u_distortion.z*r2*r2*r2) /
                           max(0.1, 1.0 + u_distortion.w*r2 + u_distortionK5*r2*r2);
            vec2 pixel = vec2(u_intrinsics.x, u_intrinsics.y) * normalized * radial + u_intrinsics.zw;
            vec2 cuv = pixel / u_imageSize;
            if (any(lessThan(cuv, vec2(0.0))) || any(greaterThan(cuv, vec2(1.0)))) continue;
            float dist2 = dot(local.xyz - voxel, local.xyz - voxel);
            float w = exp(-dist2 / 1.125); // sigma=0.75m
            vec3 rgb = cameraRgb(cuv);
            float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
            if (lum < 0.005 || lum > 0.98) continue;
            sum += rgb * w;
            weightSum += w;
        }
    }
    if (weightSum < 0.05) {
        imageStore(u_output, id, u_globalLight);
        return;
    }
    vec3 mean = sum / weightSum;
    float lum = max(dot(mean, vec3(0.2126, 0.7152, 0.0722)), 0.001);
    vec4 estimate = vec4(mean / lum, lum);
    if (u_hasPrevious != 0) estimate = mix(estimate, imageLoad(u_output, id), u_temporalSmoothing);
    imageStore(u_output, id, estimate);
}
)glsl";
    GLuint shader = CompileShader(GL_COMPUTE_SHADER, source);
    if (!shader) return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);
    glDeleteShader(shader);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]{};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("Light field program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

void SetMatrix(GLuint program, const char* name, const OVR::Matrix4f& matrix) {
    glUniformMatrix4fv(glGetUniformLocation(program, name), 1, GL_TRUE, &matrix.M[0][0]);
}

void UploadPlane(GLuint& texture, int& oldWidth, int& oldHeight,
                 int width, int height, const std::vector<uint8_t>& pixels) {
    if (!texture) glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (oldWidth != width || oldHeight != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
        oldWidth = width;
        oldHeight = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
    }
}
} // namespace

struct CameraLightEstimationPlatformState {
    std::mutex mutex;
    CameraFrame latestFrame;
    uint64_t consumedSequence = 0;
#if defined(__ANDROID__)
    ACameraManager* manager = nullptr;
    ACameraDevice* device = nullptr;
    ACameraCaptureSession* session = nullptr;
    ACaptureRequest* request = nullptr;
    ACameraOutputTarget* target = nullptr;
    ACaptureSessionOutputContainer* outputs = nullptr;
    ACaptureSessionOutput* output = nullptr;
    AImageReader* reader = nullptr;
    ANativeWindow* window = nullptr;
    std::string cameraId;
#endif
    float intrinsics[5] = {};
    float distortion[5] = {};
    float lensRotation[4] = {0, 0, 0, 1};
    float lensTranslation[3] = {};
    int activeArray[4] = {};
    bool calibrationValid = false;
    bool captureRunning = false;
    bool startAttempted = false;
    bool cameraCapabilityKnown = false;
    bool cameraAvailable = false;
};

#if defined(__ANDROID__)
namespace {
constexpr uint32_t META_CAMERA_SOURCE_TAG = 0x80004d00;
constexpr uint32_t META_CAMERA_POSITION_TAG = 0x80004d01;

void CopyPlane(AImage* image, int plane, int width, int height, std::vector<uint8_t>& destination) {
    uint8_t* data = nullptr;
    int length = 0, rowStride = 0, pixelStride = 0;
    if (AImage_getPlaneData(image, plane, &data, &length) != AMEDIA_OK || !data ||
        AImage_getPlaneRowStride(image, plane, &rowStride) != AMEDIA_OK ||
        AImage_getPlanePixelStride(image, plane, &pixelStride) != AMEDIA_OK) return;
    destination.resize(static_cast<size_t>(width * height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int source = y * rowStride + x * pixelStride;
            destination[y * width + x] = source < length ? data[source] : 0;
        }
    }
}

void OnImageAvailable(void* context, AImageReader* reader) {
    auto* platform = static_cast<CameraLightEstimationPlatformState*>(context);
    AImage* image = nullptr;
    if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || !image) return;
    CameraFrame frame;
    AImage_getWidth(image, &frame.width);
    AImage_getHeight(image, &frame.height);
    AImage_getTimestamp(image, &frame.timestampNs);
    CopyPlane(image, 0, frame.width, frame.height, frame.planes[0]);
    CopyPlane(image, 1, (frame.width + 1) / 2, (frame.height + 1) / 2, frame.planes[1]);
    CopyPlane(image, 2, (frame.width + 1) / 2, (frame.height + 1) / 2, frame.planes[2]);
    AImage_delete(image);
    std::lock_guard<std::mutex> lock(platform->mutex);
    frame.sequence = platform->latestFrame.sequence + 1;
    platform->latestFrame = std::move(frame);
}

void ConfigureCapture(CameraLightEstimationPlatformState* p) {
    if (!p->device || !p->window || p->session) return;
    if (ACameraDevice_createCaptureRequest(p->device, TEMPLATE_PREVIEW, &p->request) != ACAMERA_OK ||
        ACameraOutputTarget_create(p->window, &p->target) != ACAMERA_OK ||
        ACaptureRequest_addTarget(p->request, p->target) != ACAMERA_OK ||
        ACaptureSessionOutputContainer_create(&p->outputs) != ACAMERA_OK ||
        ACaptureSessionOutput_create(p->window, &p->output) != ACAMERA_OK ||
        ACaptureSessionOutputContainer_add(p->outputs, p->output) != ACAMERA_OK) {
        LOGE("Unable to configure headset camera output");
        return;
    }
    ACameraCaptureSession_stateCallbacks callbacks{};
    callbacks.context = p;
    if (ACameraDevice_createCaptureSession(p->device, p->outputs, &callbacks, &p->session) == ACAMERA_OK &&
        ACameraCaptureSession_setRepeatingRequest(p->session, nullptr, 1, &p->request, nullptr) == ACAMERA_OK) {
        p->captureRunning = true;
        LOGI("Headset camera capture started");
    }
}

void OnCameraDisconnected(void* context, ACameraDevice*) {
    auto* platform = static_cast<CameraLightEstimationPlatformState*>(context);
    platform->captureRunning = false;
    platform->cameraCapabilityKnown = true;
    platform->cameraAvailable = false;
}
void OnCameraError(void* context, ACameraDevice*, int error) {
    auto* platform = static_cast<CameraLightEstimationPlatformState*>(context);
    platform->captureRunning = false;
    platform->cameraCapabilityKnown = true;
    platform->cameraAvailable = false;
    LOGE("Headset camera error %d", error);
}

bool ReadFloatArray(const ACameraMetadata* metadata, uint32_t tag, float* output, uint32_t count) {
    ACameraMetadata_const_entry entry{};
    if (ACameraMetadata_getConstEntry(metadata, tag, &entry) != ACAMERA_OK || entry.count < count) return false;
    std::copy(entry.data.f, entry.data.f + count, output);
    return true;
}

void StopCamera(CameraLightEstimationPlatformState& p) {
    p.captureRunning = false;
    if (p.session) { ACameraCaptureSession_stopRepeating(p.session); ACameraCaptureSession_close(p.session); p.session = nullptr; }
    if (p.request && p.target) ACaptureRequest_removeTarget(p.request, p.target);
    if (p.output && p.outputs) ACaptureSessionOutputContainer_remove(p.outputs, p.output);
    if (p.output) { ACaptureSessionOutput_free(p.output); p.output = nullptr; }
    if (p.outputs) { ACaptureSessionOutputContainer_free(p.outputs); p.outputs = nullptr; }
    if (p.target) { ACameraOutputTarget_free(p.target); p.target = nullptr; }
    if (p.request) { ACaptureRequest_free(p.request); p.request = nullptr; }
    if (p.device) { ACameraDevice_close(p.device); p.device = nullptr; }
    if (p.reader) { AImageReader_delete(p.reader); p.reader = nullptr; p.window = nullptr; }
    if (p.manager) { ACameraManager_delete(p.manager); p.manager = nullptr; }
}

bool StartCamera(CameraLightEstimationPlatformState& p) {
    p.manager = ACameraManager_create();
    if (!p.manager) return false;
    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(p.manager, &ids) != ACAMERA_OK || !ids) return false;
    int selectedWidth = 0, selectedHeight = 0;
    for (int i = 0; i < ids->numCameras; ++i) {
        ACameraMetadata* metadata = nullptr;
        if (ACameraManager_getCameraCharacteristics(p.manager, ids->cameraIds[i], &metadata) != ACAMERA_OK) continue;
        ACameraMetadata_const_entry source{}, position{};
        const bool isRgb = ACameraMetadata_getConstEntry(metadata, META_CAMERA_SOURCE_TAG, &source) == ACAMERA_OK &&
                source.count && source.data.u8[0] == 0;
        const bool isLeft = ACameraMetadata_getConstEntry(metadata, META_CAMERA_POSITION_TAG, &position) == ACAMERA_OK &&
                position.count && position.data.u8[0] == 0;
        if (isRgb && isLeft) {
            p.cameraId = ids->cameraIds[i];
            ACameraMetadata_const_entry poseReference{};
            ACameraMetadata_const_entry timestampSource{};
            const bool gyroReferenced = ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_POSE_REFERENCE, &poseReference) == ACAMERA_OK &&
                    poseReference.count && poseReference.data.u8[0] == ACAMERA_LENS_POSE_REFERENCE_GYROSCOPE;
            const bool realtimeTimestamps = ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE, &timestampSource) == ACAMERA_OK &&
                    timestampSource.count && timestampSource.data.u8[0] == ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
            ACameraMetadata_const_entry activeArray{};
            const bool hasActiveArray = ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE, &activeArray) == ACAMERA_OK && activeArray.count >= 4;
            if (hasActiveArray) std::copy(activeArray.data.i32, activeArray.data.i32 + 4, p.activeArray);
            p.calibrationValid = gyroReferenced && realtimeTimestamps && hasActiveArray && ReadFloatArray(metadata, ACAMERA_LENS_INTRINSIC_CALIBRATION, p.intrinsics, 5) &&
                    ReadFloatArray(metadata, ACAMERA_LENS_POSE_ROTATION, p.lensRotation, 4) &&
                    ReadFloatArray(metadata, ACAMERA_LENS_POSE_TRANSLATION, p.lensTranslation, 3);
            ReadFloatArray(metadata, ACAMERA_LENS_DISTORTION, p.distortion, 5);
            ACameraMetadata_const_entry streams{};
            if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &streams) == ACAMERA_OK) {
                for (uint32_t s = 0; s + 3 < streams.count; s += 4) {
                    const int format = streams.data.i32[s], width = streams.data.i32[s + 1];
                    const int height = streams.data.i32[s + 2], input = streams.data.i32[s + 3];
                    if (format == AIMAGE_FORMAT_YUV_420_888 && input == 0 && width >= 640 && width <= 1280 &&
                        (selectedWidth == 0 || width * height < selectedWidth * selectedHeight)) {
                        selectedWidth = width; selectedHeight = height;
                    }
                }
            }
        }
        ACameraMetadata_free(metadata);
        if (!p.cameraId.empty()) break;
    }
    ACameraManager_deleteCameraIdList(ids);
    if (p.cameraId.empty() || !p.calibrationValid || selectedWidth == 0) {
        LOGW("No calibrated left passthrough RGB camera was found");
        return false;
    }
    const float activeWidth = static_cast<float>(p.activeArray[2] - p.activeArray[0]);
    const float activeHeight = static_cast<float>(p.activeArray[3] - p.activeArray[1]);
    const float outputAspect = static_cast<float>(selectedWidth) / static_cast<float>(selectedHeight);
    float cropWidth = activeWidth, cropHeight = activeHeight;
    if (activeWidth / activeHeight > outputAspect) cropWidth = activeHeight * outputAspect;
    else cropHeight = activeWidth / outputAspect;
    const float cropLeft = p.activeArray[0] + (activeWidth - cropWidth) * 0.5f;
    const float cropTop = p.activeArray[1] + (activeHeight - cropHeight) * 0.5f;
    p.intrinsics[0] *= selectedWidth / cropWidth;
    p.intrinsics[1] *= selectedHeight / cropHeight;
    p.intrinsics[2] = (p.intrinsics[2] - cropLeft) * selectedWidth / cropWidth;
    p.intrinsics[3] = (p.intrinsics[3] - cropTop) * selectedHeight / cropHeight;
    if (AImageReader_new(selectedWidth, selectedHeight, AIMAGE_FORMAT_YUV_420_888, 2, &p.reader) != AMEDIA_OK ||
        AImageReader_getWindow(p.reader, &p.window) != AMEDIA_OK) return false;
    AImageReader_ImageListener listener{&p, OnImageAvailable};
    AImageReader_setImageListener(p.reader, &listener);
    ACameraDevice_StateCallbacks callbacks{&p, OnCameraDisconnected, OnCameraError};
    const camera_status_t status = ACameraManager_openCamera(p.manager, p.cameraId.c_str(), &callbacks, &p.device);
    if (status == ACAMERA_OK) ConfigureCapture(&p);
    LOGI("Selected left passthrough camera %s (%dx%d)", p.cameraId.c_str(), selectedWidth, selectedHeight);
    return status == ACAMERA_OK && p.captureRunning;
}
} // namespace
#endif

CameraLightEstimationSystem::CameraLightEstimationSystem(XrInstance instance) : instance_(instance) {}

bool CameraLightEstimationSystem::Init(EntityManager& ecs) {
    ecs.ForEach<CameraLightEstimationState>([](EntityID, CameraLightEstimationState& state) {
        state.platform = std::make_shared<CameraLightEstimationPlatformState>();
    });
    return true;
}

void CameraLightEstimationSystem::SessionInit(EntityManager& ecs, XrSession) {
    ecs.ForEach<CameraLightEstimationState>([](EntityID, CameraLightEstimationState& state) {
        if (state.platform) state.platform->startAttempted = false;
    });
}

void CameraLightEstimationSystem::Update(
        EntityManager& ecs, const OVRFW::ovrApplFrameIn& in, bool focused) {
    CoreComponent* coreComponent = nullptr; CoreState* coreState = nullptr;
    EnvironmentDepthState* depth = nullptr; TransformState* transform = nullptr;
    FrameLoaderComponent* loader = nullptr;
    ecs.ForEachMulti<CoreComponent, CoreState>([&](EntityID, CoreComponent& c, CoreState& s) { coreComponent=&c; coreState=&s; });
    ecs.ForEach<EnvironmentDepthState>([&](EntityID, EnvironmentDepthState& d) { depth=&d; });
    ecs.ForEach<TransformState>([&](EntityID, TransformState& t) { transform=&t; });
    ecs.ForEach<FrameLoaderComponent>([&](EntityID, FrameLoaderComponent& f) { loader=&f; });
    ecs.ForEachMulti<CameraLightEstimationComponent, CameraLightEstimationState>(
        [&](EntityID, CameraLightEstimationComponent& component, CameraLightEstimationState& state) {
            const double now = NowSeconds();
            const bool spatialPrerequisitesSupported =
                coreComponent && coreState && coreComponent->supportsDepth &&
                coreComponent->supportsTimeConversion &&
                coreState->XrConvertTimespecTimeToTimeKHR &&
                coreState->viewSpace != XR_NULL_HANDLE && depth && depth->IsInitialized && transform;
#if defined(__ANDROID__)
            const bool datasetReferenceAvailable = loader && loader->dataset.HasColorReference();
            if (!datasetReferenceAvailable) {
                state.globalAvailability = TierAvailability::Unavailable;
            } else if (state.platform && state.platform->cameraCapabilityKnown) {
                state.globalAvailability = state.platform->cameraAvailable
                    ? TierAvailability::Available : TierAvailability::Unavailable;
            } else {
                state.globalAvailability = TierAvailability::Checking;
            }
#else
            state.globalAvailability = TierAvailability::Unavailable;
#endif
            if (!spatialPrerequisitesSupported ||
                state.globalAvailability == TierAvailability::Unavailable) {
                state.spatialAvailability = TierAvailability::Unavailable;
            } else if (state.globalAvailability == TierAvailability::Checking) {
                state.spatialAvailability = TierAvailability::Checking;
            } else {
                state.spatialAvailability = TierAvailability::Available;
            }

            const ColorMatchingTier requestedBeforeDowngrade = component.requestedTier;
            component.requestedTier = DowngradeUnavailableTier(
                component.requestedTier, state.globalAvailability, state.spatialAvailability);
            if (component.requestedTier != requestedBeforeDowngrade) {
                LOGW("Color matching tier auto-downgraded from %s to %s",
                    ColorMatchingTierName(requestedBeforeDowngrade),
                    ColorMatchingTierName(component.requestedTier));
            }
            if (component.requestedTier != state.loggedRequestedTier) {
                LOGI("Color matching requested tier: %s",
                    ColorMatchingTierName(component.requestedTier));
                state.loggedRequestedTier = component.requestedTier;
            }
            if (state.tier != state.loggedTier) {
                LOGI("Color matching tier: %s",
                     state.tier == LightEstimateTier::Spatial ? "spatial" :
                     state.tier == LightEstimateTier::Global ? "global" : "unavailable");
                state.loggedTier = state.tier;
            }
            if (state.lastEstimateSeconds > 0.0 && now - state.lastEstimateSeconds > component.estimateHoldSeconds) {
                state.tier = LightEstimateTier::Unavailable;
            }
#if defined(__ANDROID__)
            if (state.platform) {
                if ((!focused || !ShouldCaptureForColorMatching(component.requestedTier)) &&
                    state.platform->manager) {
                    StopCamera(*state.platform);
                    state.platform->startAttempted = false;
                } else if (focused && ShouldCaptureForColorMatching(component.requestedTier) &&
                    !state.platform->manager && !state.platform->startAttempted) {
                    state.platform->startAttempted = true;
                    const bool started = StartCamera(*state.platform);
                    state.platform->cameraCapabilityKnown = true;
                    state.platform->cameraAvailable = started;
                    state.globalAvailability = started
                        ? TierAvailability::Available : TierAvailability::Unavailable;
                    if (!started) StopCamera(*state.platform);
                }
            }
#endif
            if (!ShouldCaptureForColorMatching(component.requestedTier)) {
                state.tier = LightEstimateTier::Unavailable;
            } else if (component.requestedTier == ColorMatchingTier::Global &&
                       state.tier == LightEstimateTier::Spatial) {
                state.tier = LightEstimateTier::Global;
            }
            state.tierBlend += (static_cast<int>(state.tier) > 0 ? 1.0f : -1.0f) *
                    std::min(1.0f, in.DeltaSeconds / 0.5f);
            state.tierBlend = std::clamp(state.tierBlend, 0.0f, 1.0f);
            if (!ShouldCaptureForColorMatching(component.requestedTier) || !focused || !state.platform) {
                state.tier = LightEstimateTier::Unavailable;
                return;
            }
            CameraFrame frame;
            {
                std::lock_guard<std::mutex> lock(state.platform->mutex);
                if (state.platform->latestFrame.sequence == state.platform->consumedSequence) return;
                frame = state.platform->latestFrame;
                state.platform->consumedSequence = frame.sequence;
            }
            if (!CameraLightMath::IsFrameFresh(frame.timestampNs, NowNanoseconds(), component.maximumFrameAgeSeconds)) return;
            if (frame.planes[0].empty() || frame.planes[1].empty() || frame.planes[2].empty()) return;

            for (int plane = 0; plane < 3; ++plane) {
                const int width = plane == 0 ? frame.width : (frame.width + 1) / 2;
                const int height = plane == 0 ? frame.height : (frame.height + 1) / 2;
                UploadPlane(state.cameraTextures[plane], state.cameraTextureWidths[plane],
                            state.cameraTextureHeights[plane], width, height, frame.planes[plane]);
            }
            state.cameraImageSize = {static_cast<float>(frame.width), static_cast<float>(frame.height)};
            state.cameraIntrinsics = {state.platform->intrinsics[0], state.platform->intrinsics[1],
                                      state.platform->intrinsics[2], state.platform->intrinsics[3]};
            state.cameraCalibrationValid = state.platform->calibrationValid;

            std::vector<float> logLuminance;
            OVR::Vector3f colorSum(0.0f); int colorCount = 0;
            for (int y = 8; y < frame.height; y += 16) for (int x = 8; x < frame.width; x += 16) {
                const float yy = frame.planes[0][y * frame.width + x] / 255.0f;
                const int cw = (frame.width + 1) / 2, cx = x / 2, cy = y / 2;
                const auto rgb = CameraLightMath::YuvToLinear(yy,
                    frame.planes[1][cy*cw+cx]/255.0f, frame.planes[2][cy*cw+cx]/255.0f, false);
                const float lum = 0.2126f*rgb.r + 0.7152f*rgb.g + 0.0722f*rgb.b;
                if (lum > 0.005f && lum < 0.98f) { logLuminance.push_back(std::log(lum)); colorSum += {rgb.r,rgb.g,rgb.b}; ++colorCount; }
            }
            if (!logLuminance.empty() && colorCount) {
                const float lum = std::exp(CameraLightMath::TrimmedMean(logLuminance));
                const OVR::Vector3f mean = colorSum / static_cast<float>(colorCount);
                const float meanLuminance = std::max(
                    0.2126f*mean.x + 0.7152f*mean.y + 0.0722f*mean.z, 0.001f);
                OVR::Vector4f target(
                    mean.x/meanLuminance, mean.y/meanLuminance, mean.z/meanLuminance, lum);
                state.globalLight = state.globalLight * component.temporalSmoothing + target * (1.0f-component.temporalSmoothing);
                state.tier = LightEstimateTier::Global; state.lastEstimateSeconds = now;
            }

            if (!AllowsSpatialColorMatching(component.requestedTier)) return;

            if (!coreComponent || !coreState || !coreComponent->supportsTimeConversion ||
                !coreState->XrConvertTimespecTimeToTimeKHR || coreState->viewSpace == XR_NULL_HANDLE ||
                !depth || !depth->HasDepth || !transform || now-state.lastDispatchSeconds < 1.0/component.updateRateHz) return;
            timespec ts{frame.timestampNs / 1000000000LL, frame.timestampNs % 1000000000LL};
            XrTime captureTime = 0;
            if (XR_FAILED(coreState->XrConvertTimespecTimeToTimeKHR(instance_, &ts, &captureTime))) return;
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            if (XR_FAILED(xrLocateSpace(coreState->viewSpace, coreState->localSpace, captureTime, &location)) ||
                (location.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) !=
                 (XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
            OVR::Posef head(OVR::Quatf(location.pose.orientation.x,location.pose.orientation.y,location.pose.orientation.z,location.pose.orientation.w),
                            OVR::Vector3f(location.pose.position.x,location.pose.position.y,location.pose.position.z));
            OVR::Posef lens(OVR::Quatf(state.platform->lensRotation[0],state.platform->lensRotation[1],state.platform->lensRotation[2],state.platform->lensRotation[3]),
                            OVR::Vector3f(state.platform->lensTranslation[0],state.platform->lensTranslation[1],state.platform->lensTranslation[2]));
            const OVR::Matrix4f androidCameraToOpenXr(
                1,0,0,0, 0,-1,0,0, 0,0,-1,0, 0,0,0,1);
            state.localFromCamera = OVR::Matrix4f(head * lens) * androidCameraToOpenXr;
            state.cameraFromLocal = state.localFromCamera.Inverted();
            const OVR::Vector3f center(transform->modelMatrix.M[0][3], transform->modelMatrix.M[1][3], transform->modelMatrix.M[2][3]);
            state.gridMinimum = center - state.gridExtent * 0.5f;
            if (!state.computeProgram) state.computeProgram = BuildComputeProgram();
            if (!state.lightFieldTexture) {
                glGenTextures(1, &state.lightFieldTexture); glBindTexture(GL_TEXTURE_3D, state.lightFieldTexture);
                glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA16F, state.GridWidth, state.GridHeight, state.GridDepth);
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            }
            if (!state.computeProgram || depth->Image.swapchainIndex >= depth->SwapchainTextures.size()) return;
            glUseProgram(state.computeProgram); glBindImageTexture(0,state.lightFieldTexture,0,GL_TRUE,0,GL_READ_WRITE,GL_RGBA16F);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D_ARRAY, depth->SwapchainTextures[depth->Image.swapchainIndex].texture); glUniform1i(glGetUniformLocation(state.computeProgram,"u_depth"),0);
            for(int p=0;p<3;++p){ glActiveTexture(GL_TEXTURE1+p); glBindTexture(GL_TEXTURE_2D,state.cameraTextures[p]); glUniform1i(glGetUniformLocation(state.computeProgram,p==0?"u_y":p==1?"u_u":"u_v"),1+p); }
            const OVR::Matrix4f depthToLocal = (depth->DepthProjectionMatrices[0] * depth->DepthViewMatrices[0]).Inverted();
            SetMatrix(state.computeProgram,"u_depthToLocal",depthToLocal); SetMatrix(state.computeProgram,"u_cameraFromLocal",state.cameraFromLocal);
            glUniform4f(glGetUniformLocation(state.computeProgram,"u_intrinsics"),state.cameraIntrinsics.x,state.cameraIntrinsics.y,state.cameraIntrinsics.z,state.cameraIntrinsics.w);
            glUniform4f(glGetUniformLocation(state.computeProgram,"u_distortion"),state.platform->distortion[0],state.platform->distortion[1],state.platform->distortion[2],state.platform->distortion[3]);
            glUniform1f(glGetUniformLocation(state.computeProgram,"u_distortionK5"),state.platform->distortion[4]);
            glUniform2f(glGetUniformLocation(state.computeProgram,"u_imageSize"),state.cameraImageSize.x,state.cameraImageSize.y);
            glUniform3f(glGetUniformLocation(state.computeProgram,"u_gridMinimum"),state.gridMinimum.x,state.gridMinimum.y,state.gridMinimum.z);
            glUniform3f(glGetUniformLocation(state.computeProgram,"u_gridExtent"),state.gridExtent.x,state.gridExtent.y,state.gridExtent.z);
            glUniform4f(glGetUniformLocation(state.computeProgram,"u_globalLight"),state.globalLight.x,state.globalLight.y,state.globalLight.z,state.globalLight.w);
            glUniform1f(glGetUniformLocation(state.computeProgram,"u_temporalSmoothing"),component.temporalSmoothing);
            glUniform1i(glGetUniformLocation(state.computeProgram,"u_hasPrevious"),state.texturesReady ? 1 : 0);
            glDispatchCompute(4,3,4); glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT|GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            state.texturesReady=true; state.tier=LightEstimateTier::Spatial; state.lastDispatchSeconds=now; state.lastEstimateSeconds=now;
        });
}

void CameraLightEstimationSystem::SessionEnd(EntityManager& ecs) {
#if defined(__ANDROID__)
    ecs.ForEach<CameraLightEstimationState>([](EntityID, CameraLightEstimationState& state) { if(state.platform) StopCamera(*state.platform); });
#endif
}

void CameraLightEstimationSystem::Shutdown(EntityManager& ecs) {
    SessionEnd(ecs);
    ecs.ForEach<CameraLightEstimationState>([](EntityID, CameraLightEstimationState& state) {
        if(state.lightFieldTexture) glDeleteTextures(1,&state.lightFieldTexture);
        glDeleteTextures(3,state.cameraTextures);
        if(state.computeProgram) glDeleteProgram(state.computeProgram);
        state = {};
    });
}
