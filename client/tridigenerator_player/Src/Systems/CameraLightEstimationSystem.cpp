#include "CameraLightEstimationSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
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
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/trace.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <sys/system_properties.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
constexpr float kCameraProcessingRateHz = 5.0f;

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

void ReplaceOnce(std::string& value, const char* from, const char* to) {
    const size_t position = value.find(from);
    if (position != std::string::npos) value.replace(position, std::strlen(from), to);
}

GLuint BuildComputeProgram(bool rawExternalYuv = false) {
    static const char* source = R"glsl(#version 310 es
precision highp float;
precision highp int;
layout(local_size_x=4, local_size_y=4, local_size_z=4) in;
layout(rgba32f, binding=0) uniform writeonly highp image3D u_output;
uniform highp sampler3D u_previous;
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
    if (u_hasPrevious != 0) {
        estimate = mix(estimate, texelFetch(u_previous, id, 0), u_temporalSmoothing);
    }
    imageStore(u_output, id, estimate);
}
)glsl";
    std::string shaderSource(source);
    if (rawExternalYuv) {
        ReplaceOnce(shaderSource, "#version 310 es\n",
            "#version 310 es\n#extension GL_EXT_YUV_target : require\n");
        ReplaceOnce(shaderSource,
            "uniform highp sampler2D u_y;\nuniform highp sampler2D u_u;\nuniform highp sampler2D u_v;",
            "uniform __samplerExternal2DY2YEXT u_camera;");
        ReplaceOnce(shaderSource,
            "    float y = texture(u_y, uv).r;\n"
            "    float uu = texture(u_u, uv).r - 0.5;\n"
            "    float vv = texture(u_v, uv).r - 0.5;",
            "    vec3 yuv = texture(u_camera, uv).xyz;\n"
            "    float y = yuv.x;\n"
            "    float uu = yuv.y - 0.5;\n"
            "    float vv = yuv.z - 0.5;");
    }
    GLuint shader = CompileShader(GL_COMPUTE_SHADER, shaderSource.c_str());
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

GLuint BuildRawSampleProgram() {
    static const char* vertex = R"glsl(#version 310 es
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";
    static const char* fragment = R"glsl(#version 310 es
#extension GL_EXT_YUV_target : require
precision highp float;
uniform __samplerExternal2DY2YEXT u_camera;
uniform vec2 u_imageSize;
layout(location=0) out vec4 outYuv;
void main() {
    vec2 pixel = vec2(8.0) + floor(gl_FragCoord.xy - vec2(0.5)) * 16.0;
    outYuv = vec4(texture(u_camera, (pixel + 0.5) / u_imageSize).xyz, 1.0);
}
)glsl";
    const GLuint vs = CompileShader(GL_VERTEX_SHADER, vertex);
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]{};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("Raw YUV sampling program link failed: %s", log);
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
    std::shared_ptr<PerformanceTimingStats> performanceTiming;
#if defined(__ANDROID__)
    CameraPipelineMode pipelineMode = CameraPipelineMode::Unavailable;
    AImage* pendingImage = nullptr;
    AImage* inFlightImage = nullptr;
    EGLImageKHR inFlightEglImage = EGL_NO_IMAGE_KHR;
    GLsync inFlightFence = nullptr;
    GLuint externalTexture = 0;
    GLuint rawComputeProgram = 0;
    GLuint rawSampleProgram = 0;
    GLuint sampleFramebuffer = 0;
    GLuint sampleTexture = 0;
    int sampleWidth = 0;
    int sampleHeight = 0;
    struct ReadbackSlot {
        GLuint pbo = 0;
        GLsync fence = nullptr;
        int width = 0;
        int height = 0;
    };
    std::array<ReadbackSlot, 3> readbacks{};
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getNativeClientBuffer = nullptr;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture = nullptr;
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
    bool requestFixed15Fps = false;
    bool forceCpuFallback = false;
    int debugPipelineOverride = 0; // 0=auto, 1=raw, 2=cpu, 3=extension failure
#endif
    std::atomic<uint64_t> callbackCount{0};
    std::atomic<uint64_t> processedCount{0};
    std::atomic<uint64_t> supersededFrameCount{0};
    std::atomic<uint64_t> queuePressureDrops{0};
    std::atomic<uint64_t> bytesCopied{0};
    std::mutex diagnosticMutex;
    std::deque<float> callbackTimesMs;
    std::deque<float> importTimesMs;
    float intrinsics[5] = {};
    float distortion[5] = {};
    float lensRotation[4] = {0, 0, 0, 1};
    float lensTranslation[3] = {};
    int activeArray[4] = {};
    bool calibrationValid = false;
    bool captureRunning = false;
    bool stopping = false;
    bool startAttempted = false;
    bool cameraCapabilityKnown = false;
    bool cameraAvailable = false;
    int consecutiveStartFailures = 0;
    double nextStartAttemptSeconds = 0.0;
    std::string lastStartError;
};

#if defined(__ANDROID__)
namespace {
constexpr uint32_t META_CAMERA_SOURCE_TAG = 0x80004d00;
constexpr uint32_t META_CAMERA_POSITION_TAG = 0x80004d01;

struct TraceSection {
    explicit TraceSection(const char* name) { ATrace_beginSection(name); }
    ~TraceSection() { ATrace_endSection(); }
};

bool HasExtension(const char* list, const char* extension) {
    if (!list || !extension || std::strchr(extension, ' ')) return false;
    const size_t length = std::strlen(extension);
    for (const char* start = list; (start = std::strstr(start, extension)); start += length) {
        if ((start == list || start[-1] == ' ') &&
            (start[length] == '\0' || start[length] == ' ')) return true;
    }
    return false;
}

void RecordLatency(std::deque<float>& samples, float milliseconds) {
    samples.push_back(milliseconds);
    if (samples.size() > 256) samples.pop_front();
}

float Percentile(const std::deque<float>& samples, float fraction) {
    if (samples.empty()) return 0.0f;
    std::vector<float> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());
    const size_t index = std::min(
        sorted.size() - 1,
        static_cast<size_t>(fraction * static_cast<float>(sorted.size() - 1)));
    return sorted[index];
}

bool ProbeRawExternalYuv(CameraLightEstimationPlatformState& p, std::string& reason) {
    const char* glExtensions =
        reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const EGLDisplay display = eglGetCurrentDisplay();
    const char* eglExtensions =
        display == EGL_NO_DISPLAY ? nullptr : eglQueryString(display, EGL_EXTENSIONS);
    const char* requiredGl[] = {"GL_EXT_YUV_target", "GL_OES_EGL_image_external"};
    const char* requiredEgl[] = {
        "EGL_KHR_image_base", "EGL_ANDROID_get_native_client_buffer",
        "EGL_ANDROID_image_native_buffer"};
    for (const char* extension : requiredGl) {
        if (!HasExtension(glExtensions, extension)) {
            reason = std::string("missing ") + extension;
            return false;
        }
    }
    for (const char* extension : requiredEgl) {
        if (!HasExtension(eglExtensions, extension)) {
            reason = std::string("missing ") + extension;
            return false;
        }
    }
    p.getNativeClientBuffer =
        reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
            eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    p.createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    p.destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    p.imageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!p.getNativeClientBuffer || !p.createImage || !p.destroyImage ||
        !p.imageTargetTexture) {
        reason = "missing EGL/GL image import function";
        return false;
    }
    if (!p.rawComputeProgram) p.rawComputeProgram = BuildComputeProgram(true);
    if (!p.rawSampleProgram) p.rawSampleProgram = BuildRawSampleProgram();
    if (!p.rawComputeProgram || !p.rawSampleProgram) {
        reason = "raw-YUV shader compilation failed";
        return false;
    }
    return true;
}

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

bool RetireImportedImage(CameraLightEstimationPlatformState& p) {
    if (!p.inFlightImage) return true;
    if (p.inFlightFence) {
        const GLenum result = glClientWaitSync(p.inFlightFence, 0, 0);
        if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) {
            return false;
        }
        glDeleteSync(p.inFlightFence);
        p.inFlightFence = nullptr;
    }
    TraceSection trace("Camera image retirement");
    if (p.inFlightEglImage != EGL_NO_IMAGE_KHR && p.destroyImage) {
        p.destroyImage(eglGetCurrentDisplay(), p.inFlightEglImage);
        p.inFlightEglImage = EGL_NO_IMAGE_KHR;
    }
    AImage_delete(p.inFlightImage);
    p.inFlightImage = nullptr;
    return true;
}

bool ImportCameraImage(
        CameraLightEstimationPlatformState& p,
        AImage* image,
        int& width,
        int& height,
        int64_t& timestampNs) {
    TraceSection trace("Camera EGL import");
    const auto start = Clock::now();
    AHardwareBuffer* hardwareBuffer = nullptr;
    AImage_getWidth(image, &width);
    AImage_getHeight(image, &height);
    AImage_getTimestamp(image, &timestampNs);
    if (AImage_getHardwareBuffer(image, &hardwareBuffer) != AMEDIA_OK ||
        !hardwareBuffer) return false;
    const EGLClientBuffer clientBuffer = p.getNativeClientBuffer(hardwareBuffer);
    if (!clientBuffer) return false;
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    p.inFlightEglImage = p.createImage(
        eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
        clientBuffer, attributes);
    if (p.inFlightEglImage == EGL_NO_IMAGE_KHR) return false;
    if (!p.externalTexture) glGenTextures(1, &p.externalTexture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, p.externalTexture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    for (int staleErrors = 0;
         staleErrors < 8 && glGetError() != GL_NO_ERROR;
         ++staleErrors) {}
    p.imageTargetTexture(GL_TEXTURE_EXTERNAL_OES, p.inFlightEglImage);
    if (glGetError() != GL_NO_ERROR) {
        p.destroyImage(eglGetCurrentDisplay(), p.inFlightEglImage);
        p.inFlightEglImage = EGL_NO_IMAGE_KHR;
        return false;
    }
    p.inFlightImage = image;
    const float elapsed = std::chrono::duration<float, std::milli>(
        Clock::now() - start).count();
    std::lock_guard<std::mutex> lock(p.diagnosticMutex);
    RecordLatency(p.importTimesMs, elapsed);
    return true;
}

void EnsureSampleTarget(CameraLightEstimationPlatformState& p, int imageWidth, int imageHeight) {
    const int width = (imageWidth + 7) / 16;
    const int height = (imageHeight + 7) / 16;
    if (p.sampleTexture && p.sampleWidth == width && p.sampleHeight == height) return;
    if (!p.sampleFramebuffer) glGenFramebuffers(1, &p.sampleFramebuffer);
    if (!p.sampleTexture) glGenTextures(1, &p.sampleTexture);
    glBindTexture(GL_TEXTURE_2D, p.sampleTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
        GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, p.sampleFramebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p.sampleTexture, 0);
    p.sampleWidth = width;
    p.sampleHeight = height;
}

void PollGlobalReadbacks(
        CameraLightEstimationPlatformState& p,
        CameraLightEstimationState& state,
        const CameraLightEstimationComponent& component,
        double now) {
    TraceSection trace("Camera global readback");
    for (auto& slot : p.readbacks) {
        if (!slot.fence) continue;
        const GLenum result = glClientWaitSync(slot.fence, 0, 0);
        if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) continue;
        glDeleteSync(slot.fence);
        slot.fence = nullptr;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
        const size_t byteCount =
            static_cast<size_t>(slot.width * slot.height * 4);
        const auto* pixels = static_cast<const uint8_t*>(
            glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, byteCount, GL_MAP_READ_BIT));
        if (!pixels) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            continue;
        }
        std::vector<float> logLuminance;
        OVR::Vector3f colorSum(0.0f);
        int colorCount = 0;
        for (int i = 0; i < slot.width * slot.height; ++i) {
            const auto rgb = CameraLightMath::YuvToLinear(
                pixels[i * 4] / 255.0f, pixels[i * 4 + 1] / 255.0f,
                pixels[i * 4 + 2] / 255.0f, false);
            const float lum =
                0.2126f * rgb.r + 0.7152f * rgb.g + 0.0722f * rgb.b;
            if (lum > 0.005f && lum < 0.98f) {
                logLuminance.push_back(std::log(lum));
                colorSum += {rgb.r, rgb.g, rgb.b};
                ++colorCount;
            }
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        if (!logLuminance.empty() && colorCount) {
            const float luminance =
                std::exp(CameraLightMath::TrimmedMean(logLuminance));
            const OVR::Vector3f mean = colorSum / static_cast<float>(colorCount);
            const float meanLuminance = std::max(
                0.2126f * mean.x + 0.7152f * mean.y + 0.0722f * mean.z,
                0.001f);
            const OVR::Vector4f target(
                mean.x / meanLuminance, mean.y / meanLuminance,
                mean.z / meanLuminance, luminance);
            state.globalLight =
                state.globalLight * component.temporalSmoothing +
                target * (1.0f - component.temporalSmoothing);
            state.tier = LightEstimateTier::Global;
            state.lastEstimateSeconds = now;
        }
    }
}

bool EnqueueGlobalReadback(
        CameraLightEstimationPlatformState& p, int imageWidth, int imageHeight) {
    TraceSection trace("Camera global sample");
    auto slot = std::find_if(
        p.readbacks.begin(), p.readbacks.end(),
        [](const CameraLightEstimationPlatformState::ReadbackSlot& item) {
            return item.fence == nullptr;
        });
    if (slot == p.readbacks.end()) return false;
    GLint previousFramebuffer = 0;
    GLint previousViewport[4]{};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    EnsureSampleTarget(p, imageWidth, imageHeight);
    glBindFramebuffer(GL_FRAMEBUFFER, p.sampleFramebuffer);
    glViewport(0, 0, p.sampleWidth, p.sampleHeight);
    glUseProgram(p.rawSampleProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, p.externalTexture);
    glUniform1i(glGetUniformLocation(p.rawSampleProgram, "u_camera"), 0);
    glUniform2f(
        glGetUniformLocation(p.rawSampleProgram, "u_imageSize"),
        static_cast<float>(imageWidth), static_cast<float>(imageHeight));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (!slot->pbo) glGenBuffers(1, &slot->pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, slot->pbo);
    const size_t byteCount =
        static_cast<size_t>(p.sampleWidth * p.sampleHeight * 4);
    glBufferData(GL_PIXEL_PACK_BUFFER, byteCount, nullptr, GL_STREAM_READ);
    glReadPixels(
        0, 0, p.sampleWidth, p.sampleHeight, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    slot->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    slot->width = p.sampleWidth;
    slot->height = p.sampleHeight;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
    glViewport(
        previousViewport[0], previousViewport[1],
        previousViewport[2], previousViewport[3]);
    return slot->fence != nullptr;
}

void DestroyCameraGlResources(CameraLightEstimationPlatformState& p) {
    RetireImportedImage(p);
    for (auto& slot : p.readbacks) {
        if (slot.fence) glDeleteSync(slot.fence);
        if (slot.pbo) glDeleteBuffers(1, &slot.pbo);
        slot = {};
    }
    if (p.sampleFramebuffer) glDeleteFramebuffers(1, &p.sampleFramebuffer);
    if (p.sampleTexture) glDeleteTextures(1, &p.sampleTexture);
    if (p.externalTexture) glDeleteTextures(1, &p.externalTexture);
    if (p.rawComputeProgram) glDeleteProgram(p.rawComputeProgram);
    if (p.rawSampleProgram) glDeleteProgram(p.rawSampleProgram);
    p.sampleFramebuffer = 0;
    p.sampleTexture = 0;
    p.externalTexture = 0;
    p.rawComputeProgram = 0;
    p.rawSampleProgram = 0;
}

void OnImageAvailable(void* context, AImageReader* reader) {
    TraceSection trace("Camera2 image callback");
    const auto callbackStart = Clock::now();
    auto* platform = static_cast<CameraLightEstimationPlatformState*>(context);
    ScopedCpuTimer captureTimer(
        platform ? platform->performanceTiming.get() : nullptr,
        PerformanceSubsystem::CameraCapture);
    if (!platform) return;
    ++platform->callbackCount;
    AImage* image = nullptr;
    if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || !image) return;
    if (platform->pipelineMode == CameraPipelineMode::RawExternalYuv) {
        std::lock_guard<std::mutex> lock(platform->mutex);
        if (platform->pendingImage) {
            AImage_delete(platform->pendingImage);
            ++platform->supersededFrameCount;
        }
        platform->pendingImage = image;
        const float elapsed = std::chrono::duration<float, std::milli>(
            Clock::now() - callbackStart).count();
        std::lock_guard<std::mutex> diagnosticLock(platform->diagnosticMutex);
        RecordLatency(platform->callbackTimesMs, elapsed);
        return;
    }
    CameraFrame frame;
    AImage_getWidth(image, &frame.width);
    AImage_getHeight(image, &frame.height);
    AImage_getTimestamp(image, &frame.timestampNs);
    CopyPlane(image, 0, frame.width, frame.height, frame.planes[0]);
    CopyPlane(image, 1, (frame.width + 1) / 2, (frame.height + 1) / 2, frame.planes[1]);
    CopyPlane(image, 2, (frame.width + 1) / 2, (frame.height + 1) / 2, frame.planes[2]);
    platform->bytesCopied += frame.planes[0].size() +
        frame.planes[1].size() + frame.planes[2].size();
    AImage_delete(image);
    std::lock_guard<std::mutex> lock(platform->mutex);
    frame.sequence = platform->latestFrame.sequence + 1;
    platform->latestFrame = std::move(frame);
    const float elapsed = std::chrono::duration<float, std::milli>(
        Clock::now() - callbackStart).count();
    std::lock_guard<std::mutex> diagnosticLock(platform->diagnosticMutex);
    RecordLatency(platform->callbackTimesMs, elapsed);
}

bool ConfigureCapture(CameraLightEstimationPlatformState* p) {
    if (!p->device || !p->window || p->session) {
        p->lastStartError = "invalid camera session state";
        return false;
    }
    camera_status_t status =
        ACameraDevice_createCaptureRequest(p->device, TEMPLATE_PREVIEW, &p->request);
    if (status != ACAMERA_OK) {
        p->lastStartError =
            "create capture request failed (" + std::to_string(status) + ")";
        return false;
    }
    if (p->requestFixed15Fps) {
        const int32_t range[2] = {15, 15};
        status = ACaptureRequest_setEntry_i32(
            p->request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, range);
        if (status != ACAMERA_OK) {
            LOGW("Could not request advertised [15,15] camera FPS range (%d); using preview default",
                status);
        }
    }
    status = ACameraOutputTarget_create(p->window, &p->target);
    if (status != ACAMERA_OK) {
        p->lastStartError =
            "create camera target failed (" + std::to_string(status) + ")";
        return false;
    }
    status = ACaptureRequest_addTarget(p->request, p->target);
    if (status != ACAMERA_OK) {
        p->lastStartError =
            "add camera target failed (" + std::to_string(status) + ")";
        return false;
    }
    status = ACaptureSessionOutputContainer_create(&p->outputs);
    if (status != ACAMERA_OK) {
        p->lastStartError =
            "create output container failed (" + std::to_string(status) + ")";
        return false;
    }
    status = ACaptureSessionOutput_create(p->window, &p->output);
    if (status != ACAMERA_OK) {
        p->lastStartError =
            "create session output failed (" + std::to_string(status) + ")";
        return false;
    }
    status = ACaptureSessionOutputContainer_add(p->outputs, p->output);
    if (status != ACAMERA_OK) {
        p->lastStartError =
            "add session output failed (" + std::to_string(status) + ")";
        return false;
    }
    ACameraCaptureSession_stateCallbacks callbacks{};
    callbacks.context = p;
    status =
        ACameraDevice_createCaptureSession(p->device, p->outputs, &callbacks, &p->session);
    if (status != ACAMERA_OK) {
        p->lastStartError =
            "create capture session failed (" + std::to_string(status) + ")";
        return false;
    }
    status = ACameraCaptureSession_setRepeatingRequest(
        p->session, nullptr, 1, &p->request, nullptr);
    if (status != ACAMERA_OK) {
        p->lastStartError =
            "start repeating capture failed (" + std::to_string(status) + ")";
        return false;
    }
    p->captureRunning = true;
    p->lastStartError.clear();
    LOGI("Headset camera capture started");
    return true;
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
    if (!p.stopping) LOGI("Stopping headset camera capture");
    p.captureRunning = false;
    p.stopping = true;

    // Prevent new image work from racing teardown. Closing the camera device is
    // synchronous and already stops its repeating request, so do that before
    // releasing any session outputs or their AImageReader surface.
    if (p.reader && p.device) {
        AImageReader_ImageListener listener{nullptr, nullptr};
        AImageReader_setImageListener(p.reader, &listener);
    }
    if (p.device) {
        ACameraDevice_close(p.device);
        p.device = nullptr;
    }
    if (p.session) {
        ACameraCaptureSession_close(p.session);
        p.session = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.pendingImage) {
            AImage_delete(p.pendingImage);
            p.pendingImage = nullptr;
        }
    }
    // Never wait on the XR thread. Keep the reader and its owning AImage alive
    // until a later update observes GPU completion.
    if (!RetireImportedImage(p)) return;
    if (p.request && p.target) ACaptureRequest_removeTarget(p.request, p.target);
    if (p.output && p.outputs) ACaptureSessionOutputContainer_remove(p.outputs, p.output);
    if (p.output) { ACaptureSessionOutput_free(p.output); p.output = nullptr; }
    if (p.outputs) { ACaptureSessionOutputContainer_free(p.outputs); p.outputs = nullptr; }
    if (p.target) { ACameraOutputTarget_free(p.target); p.target = nullptr; }
    if (p.request) { ACaptureRequest_free(p.request); p.request = nullptr; }
    if (p.reader) { AImageReader_delete(p.reader); p.reader = nullptr; p.window = nullptr; }
    if (p.manager) { ACameraManager_delete(p.manager); p.manager = nullptr; }
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.consumedSequence = p.latestFrame.sequence;
    }
    p.pipelineMode = CameraPipelineMode::Unavailable;
    p.stopping = false;
    LOGI("Headset camera capture stopped");
}

bool StartCamera(CameraLightEstimationPlatformState& p) {
    p.lastStartError.clear();
    p.cameraId.clear();
    p.calibrationValid = false;
    p.stopping = false;
    p.requestFixed15Fps = false;
    char overrideValue[PROP_VALUE_MAX]{};
    __system_property_get("debug.tridi.camera_pipeline", overrideValue);
    const std::string pipelineOverride(overrideValue);
    p.debugPipelineOverride =
        pipelineOverride == "raw" ? 1 :
        pipelineOverride == "cpu" ? 2 :
        pipelineOverride == "extension-failure" ? 3 : 0;
    std::fill_n(p.intrinsics, 5, 0.0f);
    std::fill_n(p.distortion, 5, 0.0f);
    std::fill_n(p.lensRotation, 4, 0.0f);
    p.lensRotation[3] = 1.0f;
    std::fill_n(p.lensTranslation, 3, 0.0f);
    std::fill_n(p.activeArray, 4, 0);
    p.manager = ACameraManager_create();
    if (!p.manager) {
        p.lastStartError = "could not create camera manager";
        return false;
    }
    ACameraIdList* ids = nullptr;
    const camera_status_t listStatus =
        ACameraManager_getCameraIdList(p.manager, &ids);
    if (listStatus != ACAMERA_OK || !ids) {
        p.lastStartError =
            "camera list failed (" + std::to_string(listStatus) + ")";
        return false;
    }
    const int cameraCount = ids->numCameras;
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
            ACameraMetadata_const_entry fpsRanges{};
            std::vector<CameraLightMath::FpsRange> advertisedFps;
            if (ACameraMetadata_getConstEntry(
                    metadata, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                    &fpsRanges) == ACAMERA_OK) {
                for (uint32_t f = 0; f + 1 < fpsRanges.count; f += 2) {
                    advertisedFps.push_back(
                        {fpsRanges.data.i32[f], fpsRanges.data.i32[f + 1]});
                    LOGI("Camera advertised FPS range [%d,%d]",
                        fpsRanges.data.i32[f], fpsRanges.data.i32[f + 1]);
                }
            }
            CameraLightMath::FpsRange selectedFps{};
            p.requestFixed15Fps = CameraLightMath::SelectExactFpsRange(
                advertisedFps, 15, selectedFps);
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
    if (p.cameraId.empty() || selectedWidth == 0) {
        p.lastStartError = p.cameraId.empty()
            ? "no passthrough RGB camera found (" +
                std::to_string(cameraCount) + " cameras)"
            : "passthrough camera has no supported YUV stream";
        LOGW("%s", p.lastStartError.c_str());
        return false;
    }
    if (p.calibrationValid) {
        const float activeWidth = static_cast<float>(p.activeArray[2] - p.activeArray[0]);
        const float activeHeight = static_cast<float>(p.activeArray[3] - p.activeArray[1]);
        const float outputAspect =
            static_cast<float>(selectedWidth) / static_cast<float>(selectedHeight);
        float cropWidth = activeWidth, cropHeight = activeHeight;
        if (activeWidth / activeHeight > outputAspect) {
            cropWidth = activeHeight * outputAspect;
        } else {
            cropHeight = activeWidth / outputAspect;
        }
        const float cropLeft = p.activeArray[0] + (activeWidth - cropWidth) * 0.5f;
        const float cropTop = p.activeArray[1] + (activeHeight - cropHeight) * 0.5f;
        p.intrinsics[0] *= selectedWidth / cropWidth;
        p.intrinsics[1] *= selectedHeight / cropHeight;
        p.intrinsics[2] = (p.intrinsics[2] - cropLeft) * selectedWidth / cropWidth;
        p.intrinsics[3] = (p.intrinsics[3] - cropTop) * selectedHeight / cropHeight;
    } else {
        // Global matching only needs image samples. Keep it available even when
        // the pose/intrinsics metadata required for spatial reprojection is absent.
        LOGW("Passthrough camera has no usable spatial calibration; global matching only");
    }
    std::string rawFallbackReason;
    const bool forceCpu =
        p.forceCpuFallback || p.debugPipelineOverride == 2 ||
        p.debugPipelineOverride == 3;
    bool rawSupported =
        !forceCpu && ProbeRawExternalYuv(p, rawFallbackReason);
    if (p.debugPipelineOverride == 3) {
        rawFallbackReason = "debug-forced extension failure";
    } else if (p.debugPipelineOverride == 2) {
        rawFallbackReason = "debug-forced CPU pipeline";
    }
    if (p.debugPipelineOverride == 1 && !rawSupported) {
        p.lastStartError =
            "debug-forced raw pipeline unavailable: " + rawFallbackReason;
        return false;
    }
    if (p.forceCpuFallback && rawFallbackReason.empty()) {
        rawFallbackReason = p.lastStartError.empty()
            ? "raw import previously failed" : p.lastStartError;
    }
    media_status_t mediaStatus = AMEDIA_ERROR_UNSUPPORTED;
    if (rawSupported) {
        mediaStatus = AImageReader_newWithUsage(
            selectedWidth, selectedHeight, AIMAGE_FORMAT_YUV_420_888,
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4, &p.reader);
        if (mediaStatus == AMEDIA_OK) {
            p.pipelineMode = CameraPipelineMode::RawExternalYuv;
        } else {
            rawFallbackReason =
                "GPU-sampled AImageReader creation failed (" +
                std::to_string(mediaStatus) + ")";
        }
    }
    if (p.pipelineMode != CameraPipelineMode::RawExternalYuv) {
        mediaStatus = AImageReader_new(
            selectedWidth, selectedHeight, AIMAGE_FORMAT_YUV_420_888, 2, &p.reader);
        if (mediaStatus == AMEDIA_OK) {
            p.pipelineMode = CameraPipelineMode::CpuYuvPlanes;
            LOGW("Camera pipeline: CPU YUV fallback (%s)",
                rawFallbackReason.empty() ? "raw path unavailable" : rawFallbackReason.c_str());
        }
    }
    if (mediaStatus != AMEDIA_OK) {
        p.lastStartError =
            "create image reader failed (" + std::to_string(mediaStatus) + ")";
        return false;
    }
    if (p.pipelineMode == CameraPipelineMode::RawExternalYuv) {
        LOGI("Camera pipeline: raw external YUV zero-copy");
    }
    mediaStatus = AImageReader_getWindow(p.reader, &p.window);
    if (mediaStatus != AMEDIA_OK) {
        p.lastStartError =
            "get image-reader window failed (" + std::to_string(mediaStatus) + ")";
        return false;
    }
    AImageReader_ImageListener listener{&p, OnImageAvailable};
    AImageReader_setImageListener(p.reader, &listener);
    ACameraDevice_StateCallbacks callbacks{&p, OnCameraDisconnected, OnCameraError};
    const camera_status_t status = ACameraManager_openCamera(p.manager, p.cameraId.c_str(), &callbacks, &p.device);
    if (status != ACAMERA_OK) {
        p.lastStartError =
            "open passthrough camera failed (" + std::to_string(status) + ")";
    } else {
        ConfigureCapture(&p);
    }
    LOGI("Selected left passthrough camera %s (%dx%d)", p.cameraId.c_str(), selectedWidth, selectedHeight);
    if (!p.captureRunning) {
        LOGE("Headset camera start failed: %s", p.lastStartError.c_str());
    }
    return p.captureRunning;
}
} // namespace
#endif

CameraLightEstimationSystem::CameraLightEstimationSystem(
        XrInstance instance,
        std::shared_ptr<PerformanceTimingStats> performanceTiming,
        GpuTimingManager* gpuTiming)
    : instance_(instance),
      performanceTiming_(std::move(performanceTiming)),
      gpuTiming_(gpuTiming) {}

bool CameraLightEstimationSystem::Init(EntityManager& ecs) {
    ecs.ForEach<CameraLightEstimationState>([&](EntityID, CameraLightEstimationState& state) {
        state.platform = std::make_shared<CameraLightEstimationPlatformState>();
        state.platform->performanceTiming = performanceTiming_;
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
                state.availabilityMessage = "Global: dataset has no color reference";
            } else if (state.platform && state.platform->cameraCapabilityKnown) {
                state.globalAvailability = state.platform->cameraAvailable
                    ? TierAvailability::Available : TierAvailability::Unavailable;
                state.availabilityMessage = state.platform->cameraAvailable
                    ? std::string()
                    : "Global: " + (
                        state.platform->lastStartError.empty()
                            ? std::string("camera unavailable")
                            : state.platform->lastStartError);
            } else {
                state.globalAvailability = TierAvailability::Checking;
                state.availabilityMessage = "Global: checking headset camera...";
            }
#else
            state.globalAvailability = TierAvailability::Unavailable;
            state.availabilityMessage = "Global: headset camera unavailable";
#endif
            const bool cameraSpatialCalibrationAvailable =
#if defined(__ANDROID__)
                state.platform && state.platform->cameraCapabilityKnown &&
                state.platform->cameraAvailable && state.platform->calibrationValid;
#else
                false;
#endif
            if (!spatialPrerequisitesSupported ||
                state.globalAvailability == TierAvailability::Unavailable) {
                state.spatialAvailability = TierAvailability::Unavailable;
            } else if (state.globalAvailability == TierAvailability::Checking) {
                state.spatialAvailability = TierAvailability::Checking;
            } else if (!cameraSpatialCalibrationAvailable) {
                state.spatialAvailability = TierAvailability::Unavailable;
            } else {
                state.spatialAvailability = TierAvailability::Available;
            }
            if (state.globalAvailability == TierAvailability::Available &&
                state.spatialAvailability == TierAvailability::Unavailable) {
                if (!spatialPrerequisitesSupported) {
                    state.availabilityMessage =
                        "Spatial: environment depth/alignment unavailable";
                } else if (!cameraSpatialCalibrationAvailable) {
                    state.availabilityMessage =
                        "Spatial: camera calibration unavailable";
                }
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
                if (!focused || !ShouldCaptureForColorMatching(component.requestedTier)) {
                    if (state.platform->manager) {
                        StopCamera(*state.platform);
                    }
                    // Permission dialogs temporarily remove XR focus. Always permit
                    // a fresh camera attempt after focus returns.
                    state.platform->startAttempted = false;
                    state.platform->nextStartAttemptSeconds = 0.0;
                } else if (focused && ShouldCaptureForColorMatching(component.requestedTier) &&
                    !state.platform->manager && !state.platform->startAttempted &&
                    now >= state.platform->nextStartAttemptSeconds) {
                    state.platform->startAttempted = true;
                    const bool started = StartCamera(*state.platform);
                    state.platform->cameraAvailable = started;
                    state.platform->consecutiveStartFailures =
                        started ? 0 : state.platform->consecutiveStartFailures + 1;
                    state.platform->cameraCapabilityKnown =
                        started || state.platform->consecutiveStartFailures >= 3;
                    state.globalAvailability = started
                        ? TierAvailability::Available :
                        state.platform->cameraCapabilityKnown
                            ? TierAvailability::Unavailable
                            : TierAvailability::Checking;
                    if (!started) {
                        StopCamera(*state.platform);
                        state.platform->startAttempted = false;
                        state.platform->nextStartAttemptSeconds =
                            now + (state.platform->cameraCapabilityKnown ? 5.0 : 1.0);
                        LOGW(
                            "Headset camera start failed (attempt %d); retrying",
                            state.platform->consecutiveStartFailures);
                    }
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
#if defined(__ANDROID__)
            const bool rawPipeline =
                state.platform &&
                state.platform->pipelineMode == CameraPipelineMode::RawExternalYuv;
            if (rawPipeline) {
                PollGlobalReadbacks(*state.platform, state, component, now);
                RetireImportedImage(*state.platform);
            }
            if (state.platform) {
                state.captureDiagnostics.pipeline = state.platform->pipelineMode;
                state.captureDiagnostics.callbackCount = state.platform->callbackCount.load();
                state.captureDiagnostics.processedCount = state.platform->processedCount.load();
                state.captureDiagnostics.supersededFrameCount =
                    state.platform->supersededFrameCount.load();
                state.captureDiagnostics.queuePressureDrops =
                    state.platform->queuePressureDrops.load();
                state.captureDiagnostics.bytesCopied =
                    state.platform->bytesCopied.load();
                std::lock_guard<std::mutex> diagnosticLock(
                    state.platform->diagnosticMutex);
                state.captureDiagnostics.callbackP50Ms =
                    Percentile(state.platform->callbackTimesMs, 0.50f);
                state.captureDiagnostics.callbackP95Ms =
                    Percentile(state.platform->callbackTimesMs, 0.95f);
                state.captureDiagnostics.importP50Ms =
                    Percentile(state.platform->importTimesMs, 0.50f);
                state.captureDiagnostics.importP95Ms =
                    Percentile(state.platform->importTimesMs, 0.95f);
            }
#else
            const bool rawPipeline = false;
#endif
            if (!ShouldCaptureForColorMatching(component.requestedTier) || !focused || !state.platform) {
                state.tier = LightEstimateTier::Unavailable;
                return;
            }
            if (!CameraLightMath::ShouldProcessUpdate(
                    now,
                    state.lastCameraProcessingSeconds,
                    kCameraProcessingRateHz)) {
                return;
            }
            CameraFrame frame;
            int frameWidth = 0;
            int frameHeight = 0;
            int64_t frameTimestampNs = 0;
#if defined(__ANDROID__)
            if (rawPipeline) {
                if (!RetireImportedImage(*state.platform)) {
                    std::lock_guard<std::mutex> lock(state.platform->mutex);
                    if (state.platform->pendingImage) {
                        AImage_delete(state.platform->pendingImage);
                        state.platform->pendingImage = nullptr;
                        ++state.platform->queuePressureDrops;
                    }
                    return;
                }
                AImage* image = nullptr;
                {
                    std::lock_guard<std::mutex> lock(state.platform->mutex);
                    image = state.platform->pendingImage;
                    state.platform->pendingImage = nullptr;
                }
                if (!image) return;
                AImage_getWidth(image, &frameWidth);
                AImage_getHeight(image, &frameHeight);
                AImage_getTimestamp(image, &frameTimestampNs);
                const int64_t currentNs = NowNanoseconds();
                state.captureDiagnostics.latestFrameAgeMs =
                    static_cast<float>(currentNs - frameTimestampNs) / 1.0e6f;
                if (!CameraLightMath::IsFrameFresh(
                        frameTimestampNs, currentNs,
                        component.maximumFrameAgeSeconds)) {
                    AImage_delete(image);
                    return;
                }
                if (!ImportCameraImage(
                        *state.platform, image, frameWidth, frameHeight,
                        frameTimestampNs)) {
                    AImage_delete(image);
                    ++state.platform->queuePressureDrops;
                    state.platform->forceCpuFallback = true;
                    state.platform->lastStartError =
                        "raw EGL image test import failed";
                    LOGE("Raw camera EGL import failed; restarting with CPU YUV fallback");
                    StopCamera(*state.platform);
                    state.platform->startAttempted = false;
                    state.platform->nextStartAttemptSeconds = now + 0.25;
                    return;
                }
            } else
#endif
            {
                std::lock_guard<std::mutex> lock(state.platform->mutex);
                if (state.platform->latestFrame.sequence == state.platform->consumedSequence) return;
                frame = state.platform->latestFrame;
                state.platform->consumedSequence = frame.sequence;
                frameWidth = frame.width;
                frameHeight = frame.height;
                frameTimestampNs = frame.timestampNs;
            }
            if (!rawPipeline &&
                !CameraLightMath::IsFrameFresh(
                    frameTimestampNs, NowNanoseconds(),
                    component.maximumFrameAgeSeconds)) return;
            if (!rawPipeline &&
                (frame.planes[0].empty() || frame.planes[1].empty() ||
                 frame.planes[2].empty())) return;
            state.lastCameraProcessingSeconds = now;
            ++state.platform->processedCount;

            ScopedGpuTimer gpuTimer(
                gpuTiming_, PerformanceSubsystem::LightEstimation);
            if (!rawPipeline) {
                for (int plane = 0; plane < 3; ++plane) {
                    const int width = plane == 0 ? frameWidth : (frameWidth + 1) / 2;
                    const int height = plane == 0 ? frameHeight : (frameHeight + 1) / 2;
                    UploadPlane(state.cameraTextures[plane], state.cameraTextureWidths[plane],
                                state.cameraTextureHeights[plane], width, height, frame.planes[plane]);
                }
            }
            state.cameraImageSize = {
                static_cast<float>(frameWidth), static_cast<float>(frameHeight)};
            state.cameraIntrinsics = {state.platform->intrinsics[0], state.platform->intrinsics[1],
                                      state.platform->intrinsics[2], state.platform->intrinsics[3]};
            state.cameraCalibrationValid = state.platform->calibrationValid;

            if (rawPipeline) {
#if defined(__ANDROID__)
                if (!EnqueueGlobalReadback(*state.platform, frameWidth, frameHeight)) {
                    ++state.platform->queuePressureDrops;
                }
#endif
            } else {
              std::vector<float> logLuminance;
              OVR::Vector3f colorSum(0.0f); int colorCount = 0;
              for (int y = 8; y < frameHeight; y += 16) for (int x = 8; x < frameWidth; x += 16) {
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
            }

            auto finishRawWork = [&]() {
#if defined(__ANDROID__)
                if (rawPipeline && state.platform->inFlightImage &&
                    !state.platform->inFlightFence) {
                    state.platform->inFlightFence =
                        glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    glFlush();
                }
#endif
            };
            if (!AllowsSpatialColorMatching(component.requestedTier)) {
                finishRawWork();
                return;
            }

            if (!state.platform->calibrationValid ||
                !coreComponent || !coreState || !coreComponent->supportsTimeConversion ||
                !coreState->XrConvertTimespecTimeToTimeKHR || coreState->viewSpace == XR_NULL_HANDLE ||
                !depth || !depth->HasDepth || !transform) {
                finishRawWork();
                return;
            }
            timespec ts{frameTimestampNs / 1000000000LL, frameTimestampNs % 1000000000LL};
            XrTime captureTime = 0;
            if (XR_FAILED(coreState->XrConvertTimespecTimeToTimeKHR(instance_, &ts, &captureTime))) {
                finishRawWork();
                return;
            }
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            if (XR_FAILED(xrLocateSpace(coreState->viewSpace, coreState->localSpace, captureTime, &location)) ||
                (location.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) !=
                 (XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                finishRawWork();
                return;
            }
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
            if (!rawPipeline && !state.computeProgram) {
                state.computeProgram = BuildComputeProgram(false);
            }
            if (!state.lightFieldTexture || !state.lightFieldScratchTexture) {
                GLuint textures[2]{};
                glGenTextures(2, textures);
                state.lightFieldTexture = textures[0];
                state.lightFieldScratchTexture = textures[1];
                for (const GLuint texture : textures) {
                    glBindTexture(GL_TEXTURE_3D, texture);
                    glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA32F,
                        state.GridWidth, state.GridHeight, state.GridDepth);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
                }
            }
            const GLuint computeProgram = rawPipeline
#if defined(__ANDROID__)
                ? state.platform->rawComputeProgram
#else
                ? 0
#endif
                : state.computeProgram;
            if (!computeProgram || depth->Image.swapchainIndex >= depth->SwapchainTextures.size()) {
                finishRawWork();
                return;
            }
#if defined(__ANDROID__)
            TraceSection spatialTrace("Camera spatial dispatch");
#endif
            glUseProgram(computeProgram);
            glBindImageTexture(0, state.lightFieldScratchTexture, 0, GL_TRUE, 0,
                GL_WRITE_ONLY, GL_RGBA32F);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D_ARRAY, depth->SwapchainTextures[depth->Image.swapchainIndex].texture); glUniform1i(glGetUniformLocation(computeProgram,"u_depth"),0);
            if (rawPipeline) {
#if defined(__ANDROID__)
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, state.platform->externalTexture);
                glUniform1i(glGetUniformLocation(computeProgram, "u_camera"), 1);
#endif
            } else {
                for(int p=0;p<3;++p){ glActiveTexture(GL_TEXTURE1+p); glBindTexture(GL_TEXTURE_2D,state.cameraTextures[p]); glUniform1i(glGetUniformLocation(computeProgram,p==0?"u_y":p==1?"u_u":"u_v"),1+p); }
            }
            glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_3D, state.lightFieldTexture);
            glUniform1i(glGetUniformLocation(computeProgram, "u_previous"), 4);
            const OVR::Matrix4f depthToLocal = (depth->DepthProjectionMatrices[0] * depth->DepthViewMatrices[0]).Inverted();
            SetMatrix(computeProgram,"u_depthToLocal",depthToLocal); SetMatrix(computeProgram,"u_cameraFromLocal",state.cameraFromLocal);
            glUniform4f(glGetUniformLocation(computeProgram,"u_intrinsics"),state.cameraIntrinsics.x,state.cameraIntrinsics.y,state.cameraIntrinsics.z,state.cameraIntrinsics.w);
            glUniform4f(glGetUniformLocation(computeProgram,"u_distortion"),state.platform->distortion[0],state.platform->distortion[1],state.platform->distortion[2],state.platform->distortion[3]);
            glUniform1f(glGetUniformLocation(computeProgram,"u_distortionK5"),state.platform->distortion[4]);
            glUniform2f(glGetUniformLocation(computeProgram,"u_imageSize"),state.cameraImageSize.x,state.cameraImageSize.y);
            glUniform3f(glGetUniformLocation(computeProgram,"u_gridMinimum"),state.gridMinimum.x,state.gridMinimum.y,state.gridMinimum.z);
            glUniform3f(glGetUniformLocation(computeProgram,"u_gridExtent"),state.gridExtent.x,state.gridExtent.y,state.gridExtent.z);
            glUniform4f(glGetUniformLocation(computeProgram,"u_globalLight"),state.globalLight.x,state.globalLight.y,state.globalLight.z,state.globalLight.w);
            glUniform1f(glGetUniformLocation(computeProgram,"u_temporalSmoothing"),component.temporalSmoothing);
            glUniform1i(glGetUniformLocation(computeProgram,"u_hasPrevious"),state.texturesReady ? 1 : 0);
            glDispatchCompute(4,3,4); glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT|GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            std::swap(state.lightFieldTexture, state.lightFieldScratchTexture);
            state.texturesReady=true; state.tier=LightEstimateTier::Spatial; state.lastDispatchSeconds=now; state.lastEstimateSeconds=now;
            finishRawWork();
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
#if defined(__ANDROID__)
        if (state.platform) DestroyCameraGlResources(*state.platform);
#endif
        if(state.lightFieldTexture) glDeleteTextures(1,&state.lightFieldTexture);
        if(state.lightFieldScratchTexture) glDeleteTextures(1,&state.lightFieldScratchTexture);
        glDeleteTextures(3,state.cameraTextures);
        if(state.computeProgram) glDeleteProgram(state.computeProgram);
        state = {};
    });
}
