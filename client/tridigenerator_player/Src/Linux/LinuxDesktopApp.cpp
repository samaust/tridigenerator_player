#include "LinuxOpenXrBackend.h"
#include "LinuxStereo.h"

#include "Data/VipeDataset.h"
#include "Videos/WebmInMemoryDemuxer.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glext.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kWindowWidth = 1920;
constexpr int kWindowHeight = 1080;
constexpr float kPi = 3.14159265358979323846f;

struct Options {
    std::filesystem::path dataDirectory = "vipe_encoded";
    std::string sequence;
    std::string backend = "desktop";
    StereoLayout stereoLayout = StereoLayout::SideBySide;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(const Vec3& value, float scale) { return {value.x * scale, value.y * scale, value.z * scale}; }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 Normalize(const Vec3& value) {
    const float length = std::sqrt(Dot(value, value));
    return length > 0.0f ? value * (1.0f / length) : Vec3{};
}

struct Mat4 {
    std::array<float, 16> m{}; // OpenGL column-major
};

Mat4 Identity() {
    Mat4 result;
    result.m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return result;
}

Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 result;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            for (int k = 0; k < 4; ++k) {
                result.m[column * 4 + row] += a.m[k * 4 + row] * b.m[column * 4 + k];
            }
        }
    }
    return result;
}

Mat4 FromRowMajor(const std::array<float, 16>& source) {
    Mat4 result;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            result.m[column * 4 + row] = source[row * 4 + column];
        }
    }
    return result;
}

Mat4 Perspective(float verticalFov, float aspect, float nearPlane, float farPlane) {
    Mat4 result;
    const float f = 1.0f / std::tan(verticalFov * 0.5f);
    result.m[0] = f / aspect;
    result.m[5] = f;
    result.m[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
    result.m[11] = -1.0f;
    result.m[14] = (2.0f * farPlane * nearPlane) / (nearPlane - farPlane);
    return result;
}

Mat4 LookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
    const Vec3 forward = Normalize(center - eye);
    const Vec3 side = Normalize(Cross(forward, up));
    const Vec3 correctedUp = Cross(side, forward);
    Mat4 result = Identity();
    result.m[0] = side.x; result.m[4] = side.y; result.m[8] = side.z;
    result.m[1] = correctedUp.x; result.m[5] = correctedUp.y; result.m[9] = correctedUp.z;
    result.m[2] = -forward.x; result.m[6] = -forward.y; result.m[10] = -forward.z;
    result.m[12] = -Dot(side, eye);
    result.m[13] = -Dot(correctedUp, eye);
    result.m[14] = Dot(forward, eye);
    return result;
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Could not open " + path.string());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open " + path.string());
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0 && !input.read(reinterpret_cast<char*>(data.data()), size)) {
        throw std::runtime_error("Could not read " + path.string());
    }
    return data;
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help") {
            std::cout << "Usage: tridigenerator_player [--data-dir PATH] [--sequence NAME] "
                         "[--backend desktop|openxr] "
                         "[--stereo-layout side-by-side|over-under]\n";
            std::exit(0);
        }
        if (i + 1 >= argc) throw std::runtime_error("Missing value after " + argument);
        const std::string value = argv[++i];
        if (argument == "--data-dir") options.dataDirectory = value;
        else if (argument == "--sequence") options.sequence = value;
        else if (argument == "--backend") options.backend = value;
        else if (argument == "--stereo-layout") {
            if (!ParseStereoLayout(value, options.stereoLayout)) {
                throw std::runtime_error(
                    "--stereo-layout must be side-by-side or over-under");
            }
        }
        else throw std::runtime_error("Unknown argument: " + argument);
    }
    if (options.backend != "desktop" && options.backend != "openxr") {
        throw std::runtime_error("--backend must be desktop or openxr");
    }
    return options;
}

std::filesystem::path SelectManifest(const Options& options) {
    if (!std::filesystem::is_directory(options.dataDirectory)) {
        throw std::runtime_error("Data directory does not exist: " + options.dataDirectory.string());
    }
    if (!options.sequence.empty()) {
        const auto path = options.dataDirectory / (options.sequence + ".json");
        if (!std::filesystem::is_regular_file(path)) {
            throw std::runtime_error("Sequence manifest does not exist: " + path.string());
        }
        return path;
    }
    std::vector<std::filesystem::path> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(options.dataDirectory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json" &&
            entry.path().filename() != "catalog.json") {
            candidates.push_back(entry.path());
        }
    }
    std::sort(candidates.begin(), candidates.end());
    if (candidates.size() != 1) {
        throw std::runtime_error(
            "Expected exactly one dataset or --sequence; found " + std::to_string(candidates.size()));
    }
    return candidates.front();
}

GLuint CompileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string error(static_cast<size_t>(length), '\0');
        glGetShaderInfoLog(shader, length, nullptr, error.data());
        throw std::runtime_error("Shader compilation failed: " + error);
    }
    return shader;
}

GLuint BuildProgram() {
    static const char* vertexSource = R"GLSL(#version 330 core
layout(location=0) in vec2 uv;
uniform usampler2D depthTexture;
uniform vec4 intrinsics;
uniform vec2 imageSize;
uniform float depthUnitsPerMetre;
uniform mat4 model;
uniform mat4 viewProjection;
out vec2 textureCoordinate;
void main() {
    uint encoded = texture(depthTexture, uv).r;
    float z = float(encoded) / depthUnitsPerMetre;
    vec2 pixel = uv * imageSize;
    float x = (pixel.x - intrinsics.z) * z / intrinsics.x;
    float y = -(pixel.y - intrinsics.w) * z / intrinsics.y;
    gl_Position = viewProjection * model * vec4(x, y, -z, 1.0);
    textureCoordinate = uv;
})GLSL";
    static const char* fragmentSource = R"GLSL(#version 330 core
in vec2 textureCoordinate;
uniform sampler2D yTexture;
uniform sampler2D uTexture;
uniform sampler2D vTexture;
uniform sampler2D maskTexture;
uniform usampler2D depthTexture;
uniform int fullRange;
uniform int maskEnabled;
out vec4 color;
void main() {
    if (texture(depthTexture, textureCoordinate).r == uint(0) ||
        (maskEnabled != 0 && texture(maskTexture, textureCoordinate).r <= 0.0)) discard;
    float y = texture(yTexture, textureCoordinate).r;
    float u = texture(uTexture, textureCoordinate).r - 0.5;
    float v = texture(vTexture, textureCoordinate).r - 0.5;
    vec3 rgb;
    if (fullRange != 0) {
        rgb = vec3(y + 1.402*v, y - 0.344136*u - 0.714136*v, y + 1.772*u);
    } else {
        float c = y - 0.0625;
        rgb = vec3(1.1643*c + 1.5958*v, 1.1643*c - 0.39173*u - 0.81290*v,
                   1.1643*c + 2.017*u);
    }
    color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
})GLSL";
    const GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) throw std::runtime_error("Shader program link failed");
    return program;
}

class DesktopWindow {
public:
    DesktopWindow() {
        display_ = XOpenDisplay(nullptr);
        if (!display_) throw std::runtime_error("Could not open X11 display");
        const int attributes[] = {
            GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
            GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
            GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8,
            GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, True, None};
        int count = 0;
        GLXFBConfig* configs = glXChooseFBConfig(display_, DefaultScreen(display_), attributes, &count);
        if (!configs || count == 0) throw std::runtime_error("No suitable GLX framebuffer config");
        config_ = configs[0];
        XVisualInfo* visual = glXGetVisualFromFBConfig(display_, config_);
        XSetWindowAttributes windowAttributes{};
        windowAttributes.colormap = XCreateColormap(
            display_, RootWindow(display_, visual->screen), visual->visual, AllocNone);
        windowAttributes.event_mask = StructureNotifyMask | KeyPressMask | KeyReleaseMask |
            ButtonPressMask | PointerMotionMask | FocusChangeMask;
        window_ = XCreateWindow(
            display_, RootWindow(display_, visual->screen), 0, 0, kWindowWidth, kWindowHeight, 0,
            visual->depth, InputOutput, visual->visual, CWColormap | CWEventMask, &windowAttributes);
        XStoreName(display_, window_, "TriDiGenerator ViPE Player");
        closeAtom_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display_, window_, &closeAtom_, 1);
        XMapWindow(display_, window_);
        static const char emptyCursorBits[] = {0};
        const Pixmap emptyCursorPixmap = XCreateBitmapFromData(
            display_, window_, emptyCursorBits, 1, 1);
        XColor emptyCursorColor{};
        invisibleCursor_ = XCreatePixmapCursor(
            display_, emptyCursorPixmap, emptyCursorPixmap,
            &emptyCursorColor, &emptyCursorColor, 0, 0);
        XFreePixmap(display_, emptyCursorPixmap);
        using CreateContext = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
        const auto createContext = reinterpret_cast<CreateContext>(
            glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));
        const int contextAttributes[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3,
            GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
        context_ = createContext
            ? createContext(display_, config_, nullptr, True, contextAttributes)
            : glXCreateNewContext(display_, config_, GLX_RGBA_TYPE, nullptr, True);
        XFree(visual);
        XFree(configs);
        if (!context_ || !glXMakeCurrent(display_, window_, context_)) {
            throw std::runtime_error("Could not create GLX OpenGL context");
        }
        using SwapInterval = void (*)(Display*, GLXDrawable, int);
        const auto swapInterval = reinterpret_cast<SwapInterval>(
            glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXSwapIntervalEXT")));
        if (swapInterval) swapInterval(display_, window_, 1);
    }

    ~DesktopWindow() {
        if (display_) {
            glXMakeCurrent(display_, None, nullptr);
            if (context_) glXDestroyContext(display_, context_);
            if (invisibleCursor_) XFreeCursor(display_, invisibleCursor_);
            if (window_) XDestroyWindow(display_, window_);
            XCloseDisplay(display_);
        }
    }

    void Swap() { glXSwapBuffers(display_, window_); }
    void SetPointerCaptured(bool captured, int centerX, int centerY) {
        if (captured) {
            XDefineCursor(display_, window_, invisibleCursor_);
            XGrabPointer(display_, window_, True, PointerMotionMask | ButtonPressMask,
                GrabModeAsync, GrabModeAsync, window_, invisibleCursor_, CurrentTime);
            XWarpPointer(display_, None, window_, 0, 0, 0, 0, centerX, centerY);
        } else {
            XUngrabPointer(display_, CurrentTime);
            XUndefineCursor(display_, window_);
        }
        XFlush(display_);
    }
    Display* DisplayHandle() const { return display_; }
    Window WindowHandle() const { return window_; }
    Atom CloseAtom() const { return closeAtom_; }

private:
    Display* display_ = nullptr;
    Window window_ = 0;
    Atom closeAtom_ = 0;
    Cursor invisibleCursor_ = 0;
    GLXFBConfig config_ = nullptr;
    GLXContext context_ = nullptr;

public:
    GLXFBConfig FramebufferConfig() const { return config_; }
    GLXContext ContextHandle() const { return context_; }
};

struct Camera {
    static constexpr Vec3 kStartPosition{0.0f, 0.0f, 1.5f};
    static constexpr float kStartYaw = -kPi * 0.5f;
    static constexpr float kStartPitch = 0.0f;

    // Keep the free camera separate from ViPE's frame-0 camera origin. Starting
    // at the same point makes a correct view rotation look like a model rotation.
    Vec3 position = kStartPosition;
    float yaw = kStartYaw;
    float pitch = kStartPitch;
    bool keys[256]{};
    bool captured = true;

    Vec3 Forward() const {
        return Normalize({std::cos(pitch) * std::cos(yaw), std::sin(pitch),
                          std::cos(pitch) * std::sin(yaw)});
    }

    void Rotate(float mouseDeltaX, float mouseDeltaY) {
        constexpr float sensitivity = 0.001f;
        yaw += mouseDeltaX * sensitivity;
        pitch = std::clamp(pitch - mouseDeltaY * sensitivity, -1.55f, 1.55f);
    }

    void Reset() {
        position = kStartPosition;
        yaw = kStartYaw;
        pitch = kStartPitch;
    }

    void Update(float seconds) {
        const Vec3 forward = Forward();
        const Vec3 right = Normalize(Cross(forward, {0, 1, 0}));
        const float speed = (keys[static_cast<unsigned char>('!')] ? 5.0f : 1.5f) * seconds;
        if (keys['w']) position = position + forward * speed;
        if (keys['s']) position = position - forward * speed;
        if (keys['a']) position = position - right * speed;
        if (keys['d']) position = position + right * speed;
        if (keys['q']) position.y -= speed;
        if (keys['e']) position.y += speed;
    }

    void UpdateOpenXr(float seconds) {
        const float speed = (keys[static_cast<unsigned char>('!')] ? 5.0f : 1.5f) * seconds;
        if (keys['w']) position.z -= speed;
        if (keys['s']) position.z += speed;
        if (keys['a']) position.x -= speed;
        if (keys['d']) position.x += speed;
        if (keys['q']) position.y -= speed;
        if (keys['e']) position.y += speed;
    }

    void ResetOpenXr() { position = {}; }
};

GLuint CreateTexture(GLenum internalFormat, int width, int height, GLenum filter) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    const GLenum sourceFormat = internalFormat == GL_R16UI ? GL_RED_INTEGER : GL_RED;
    const GLenum sourceType = internalFormat == GL_R16UI ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, sourceFormat, sourceType, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

void UploadFrame(const VideoFrame& frame, const std::array<GLuint, 5>& textures) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const auto upload8 = [&](int unit, int width, int height, const uint8_t* data) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, textures[unit]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, data);
    };
    upload8(0, frame.textureYWidth, frame.textureYHeight, frame.textureYData.data());
    upload8(1, frame.textureUWidth, frame.textureUHeight, frame.textureUData.data());
    upload8(2, frame.textureVWidth, frame.textureVHeight, frame.textureVData.data());
    upload8(3, frame.textureAlphaWidth, frame.textureAlphaHeight, frame.textureAlphaData.data());
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, textures[4]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.textureDepthWidth, frame.textureDepthHeight,
        GL_RED_INTEGER, GL_UNSIGNED_SHORT, frame.textureDepthData.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

int Run(int argc, char** argv) {
    const Options options = ParseOptions(argc, argv);
    const std::filesystem::path manifestPath = SelectManifest(options);
    VipeDataset dataset;
    std::string parseError;
    if (!ParseVipeDataset(ReadText(manifestPath), dataset, parseError)) {
        throw std::runtime_error("Invalid ViPE manifest " + manifestPath.string() + ": " + parseError);
    }
    const std::filesystem::path videoPath = manifestPath.parent_path() / dataset.videoFile;
    std::vector<uint8_t> video = ReadBytes(videoPath);
    WebmInMemoryDemuxer decoder(video);
    std::string decoderError;
    if (!decoder.init(&decoderError)) throw std::runtime_error("Decoder initialization failed: " + decoderError);

    DesktopWindow window;
    std::unique_ptr<LinuxOpenXrBackend> openXr;
    if (options.backend == "openxr") {
        openXr = std::make_unique<LinuxOpenXrBackend>();
        std::string error;
        if (!openXr->Initialize(
                window.DisplayHandle(), window.FramebufferConfig(), window.WindowHandle(),
                window.ContextHandle(), error)) {
            throw std::runtime_error(error + ". Use --backend desktop if no compatible runtime is available.");
        }
    }
    const GLuint program = BuildProgram();
    std::vector<float> vertices;
    vertices.reserve(static_cast<size_t>(dataset.width) * dataset.height * 2);
    for (int y = 0; y < dataset.height; ++y) {
        for (int x = 0; x < dataset.width; ++x) {
            vertices.push_back(static_cast<float>(x) / static_cast<float>(dataset.width - 1));
            vertices.push_back(static_cast<float>(y) / static_cast<float>(dataset.height - 1));
        }
    }
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(dataset.width - 1) * (dataset.height - 1) * 6);
    for (int y = 0; y < dataset.height - 1; ++y) {
        for (int x = 0; x < dataset.width - 1; ++x) {
            const uint32_t topLeft = static_cast<uint32_t>(y * dataset.width + x);
            const uint32_t bottomLeft = topLeft + dataset.width;
            indices.insert(indices.end(), {topLeft, bottomLeft, topLeft + 1,
                                           topLeft + 1, bottomLeft, bottomLeft + 1});
        }
    }
    GLuint vao = 0, vbo = 0, ebo = 0;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenBuffers(1, &ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);

    VideoFrame frame;
    if (!decoder.decode_next_frame(frame)) throw std::runtime_error("Video contains no complete frame");
    std::array<GLuint, 5> textures{
        CreateTexture(GL_R8, frame.textureYWidth, frame.textureYHeight, GL_LINEAR),
        CreateTexture(GL_R8, frame.textureUWidth, frame.textureUHeight, GL_LINEAR),
        CreateTexture(GL_R8, frame.textureVWidth, frame.textureVHeight, GL_LINEAR),
        CreateTexture(GL_R8, frame.textureAlphaWidth, frame.textureAlphaHeight, GL_NEAREST),
        CreateTexture(GL_R16UI, frame.textureDepthWidth, frame.textureDepthHeight, GL_NEAREST)};
    UploadFrame(frame, textures);
    glUseProgram(program);
    const char* samplers[] = {"yTexture", "uTexture", "vTexture", "maskTexture", "depthTexture"};
    for (int i = 0; i < 5; ++i) glUniform1i(glGetUniformLocation(program, samplers[i]), i);
    glEnable(GL_DEPTH_TEST);
    GLuint xrFramebuffer = 0;
    GLuint xrDepthBuffer = 0;
    int xrDepthWidth = 0;
    int xrDepthHeight = 0;
    glGenFramebuffers(1, &xrFramebuffer);
    glGenRenderbuffers(1, &xrDepthBuffer);

    Camera camera;
    if (openXr) camera.ResetOpenXr();
    bool running = true;
    bool escapeReleased = false;
    bool videoPaused = false;
    bool spaceDown = false;
    bool centerDown = false;
    bool maskEnabled = true;
    bool maskDown = false;
    int windowWidth = kWindowWidth, windowHeight = kWindowHeight;
    int centerX = windowWidth / 2, centerY = windowHeight / 2;
    camera.captured = !openXr;
    escapeReleased = static_cast<bool>(openXr);
    window.SetPointerCaptured(camera.captured, centerX, centerY);
    XSync(window.DisplayHandle(), False);
    auto previous = std::chrono::steady_clock::now();
    auto nextVideoFrame = previous;
    const std::chrono::duration<double> videoPeriod(
        static_cast<double>(dataset.frameRateDenominator) / dataset.frameRateNumerator);

    while (running) {
        if (openXr && openXr->ExitRequested()) running = false;
        while (XPending(window.DisplayHandle())) {
            XEvent event{};
            XNextEvent(window.DisplayHandle(), &event);
            if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == window.CloseAtom()) {
                running = false;
            } else if (event.type == ConfigureNotify) {
                windowWidth = std::max(1, event.xconfigure.width);
                windowHeight = std::max(1, event.xconfigure.height);
                glViewport(0, 0, windowWidth, windowHeight);
                centerX = windowWidth / 2;
                centerY = windowHeight / 2;
                if (camera.captured) {
                    XWarpPointer(window.DisplayHandle(), None, window.WindowHandle(),
                        0, 0, 0, 0, centerX, centerY);
                }
            } else if (!openXr && event.type == ButtonPress &&
                       event.xbutton.button == Button1 && !camera.captured) {
                camera.captured = true; escapeReleased = false;
                window.SetPointerCaptured(true, centerX, centerY);
            } else if (event.type == MotionNotify && camera.captured) {
                // Collapse queued absolute motion to its latest position. This is
                // essential when recentering: applying every intermediate event
                // would count an increasingly large center-relative delta more
                // than once and make fast motion accelerate unexpectedly.
                XEvent latestMotion = event;
                XEvent queuedMotion{};
                while (XCheckTypedWindowEvent(
                        window.DisplayHandle(), window.WindowHandle(),
                        MotionNotify, &queuedMotion)) {
                    latestMotion = queuedMotion;
                }
                const int deltaX = latestMotion.xmotion.x - centerX;
                const int deltaY = latestMotion.xmotion.y - centerY;
                if (deltaX != 0 || deltaY != 0) {
                    camera.Rotate(static_cast<float>(deltaX), static_cast<float>(deltaY));
                    XWarpPointer(window.DisplayHandle(), None, window.WindowHandle(),
                        0, 0, 0, 0, centerX, centerY);
                    XFlush(window.DisplayHandle());
                }
            } else if (event.type == KeyPress || event.type == KeyRelease) {
                const bool down = event.type == KeyPress;
                const KeySym symbol = XLookupKeysym(&event.xkey, 0);
                if (symbol == XK_Escape && down) {
                    if (camera.captured) {
                        camera.captured = false;
                        escapeReleased = true;
                        window.SetPointerCaptured(false, centerX, centerY);
                    } else if (escapeReleased) running = false;
                }
                if (symbol == XK_space) {
                    if (down && !spaceDown) {
                        videoPaused = !videoPaused;
                        if (!videoPaused) {
                            nextVideoFrame = std::chrono::steady_clock::now() +
                                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    videoPeriod);
                        }
                        std::cerr << (videoPaused ? "Video paused\n" : "Video resumed\n");
                    }
                    spaceDown = down;
                }
                if (symbol == XK_Shift_L || symbol == XK_Shift_R) camera.keys['!'] = down;
                const char key = symbol <= 255
                    ? static_cast<char>(std::tolower(static_cast<unsigned char>(symbol))) : '\0';
                if (key == 'c') {
                    if (down && !centerDown) {
                        if (openXr) camera.ResetOpenXr();
                        else camera.Reset();
                    }
                    centerDown = down;
                }
                if (key == 'm') {
                    if (down && !maskDown) {
                        maskEnabled = !maskEnabled;
                        std::cerr << (maskEnabled ? "Mask enabled\n" : "Mask disabled\n");
                    }
                    maskDown = down;
                }
                if (key == 'w' || key == 'a' || key == 's' || key == 'd' || key == 'q' || key == 'e') {
                    camera.keys[static_cast<unsigned char>(key)] = down;
                }
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const float delta = std::chrono::duration<float>(now - previous).count();
        previous = now;
        if (openXr) camera.UpdateOpenXr(std::min(delta, 0.1f));
        else camera.Update(std::min(delta, 0.1f));
        if (!videoPaused && now >= nextVideoFrame) {
            if (!decoder.decode_next_frame(frame)) {
                if (!decoder.seek_to_start() || !decoder.decode_next_frame(frame)) {
                    throw std::runtime_error("Could not loop video");
                }
            }
            UploadFrame(frame, textures);
            nextVideoFrame = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(videoPeriod);
        }
        const int frameIndex = std::clamp(frame.frameIndex, 0, dataset.frameCount - 1);
        const auto relative = OrientedRelativeOpenGlCameraPose(
            dataset.frames[frameIndex].cameraToWorld,
            dataset.frames.front().cameraToWorld,
            dataset.orientationOffsetDegrees);
        const Mat4 model = FromRowMajor(relative);
        const auto& intrinsics = dataset.frames[frameIndex].intrinsics;
        const auto renderScene = [&](int renderWidth, int renderHeight,
                                     const Mat4& view, const Mat4& projection) {
            const Mat4 viewProjection = Multiply(projection, view);
            glViewport(0, 0, renderWidth, renderHeight);
            glClearColor(0.02f, 0.02f, 0.025f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glUseProgram(program);
            glUniformMatrix4fv(glGetUniformLocation(program, "model"), 1, GL_FALSE, model.m.data());
            glUniformMatrix4fv(glGetUniformLocation(program, "viewProjection"), 1, GL_FALSE, viewProjection.m.data());
            glUniform4fv(glGetUniformLocation(program, "intrinsics"), 1, intrinsics.data());
            glUniform2f(glGetUniformLocation(program, "imageSize"), dataset.width, dataset.height);
            glUniform1f(glGetUniformLocation(program, "depthUnitsPerMetre"), dataset.depthUnitsPerMetre);
            glUniform1i(glGetUniformLocation(program, "fullRange"), frame.yuvFullRange ? 1 : 0);
            glUniform1i(glGetUniformLocation(program, "maskEnabled"), maskEnabled ? 1 : 0);
            glBindVertexArray(vao);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, nullptr);
        };
        if (openXr) {
            LinuxOpenXrBackend::Frame xrFrame;
            std::string error;
            const bool shouldRenderXr = openXr->BeginFrame(xrFrame, error);
            if (!error.empty()) throw std::runtime_error(error);
            if (shouldRenderXr) {
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                glViewport(0, 0, windowWidth, windowHeight);
                glClearColor(0.02f, 0.02f, 0.025f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                const auto mirrorViewports = PackedStereoViewports(
                    options.stereoLayout, windowWidth, windowHeight);
                glBindFramebuffer(GL_FRAMEBUFFER, xrFramebuffer);
                if (xrFrame.width != xrDepthWidth || xrFrame.height != xrDepthHeight) {
                    xrDepthWidth = xrFrame.width; xrDepthHeight = xrFrame.height;
                    glBindRenderbuffer(GL_RENDERBUFFER, xrDepthBuffer);
                    glRenderbufferStorage(
                        GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, xrFrame.width, xrFrame.height);
                    glFramebufferRenderbuffer(
                        GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, xrDepthBuffer);
                }
                for (uint32_t eye = 0; eye < xrFrame.views.size(); ++eye) {
                    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                        xrFrame.colorTexture, 0, static_cast<GLint>(eye));
                    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                        throw std::runtime_error("OpenXR stereo framebuffer is incomplete");
                    }
                    Mat4 eyeView;
                    eyeView.m = OpenXrView(xrFrame.views[eye].pose,
                        {camera.position.x, camera.position.y, camera.position.z});
                    Mat4 eyeProjection;
                    eyeProjection.m = OpenXrProjection(
                        xrFrame.views[eye].fov, 0.01f, 1000.0f);
                    renderScene(xrFrame.width, xrFrame.height, eyeView, eyeProjection);

                    const StereoViewport& target = mirrorViewports[eye];
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, xrFramebuffer);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                    glBlitFramebuffer(0, 0, xrFrame.width, xrFrame.height,
                        target.x, target.y, target.x + target.width, target.y + target.height,
                        GL_COLOR_BUFFER_BIT, GL_LINEAR);
                    glBindFramebuffer(GL_FRAMEBUFFER, xrFramebuffer);
                }
            }
            if (!openXr->EndFrame(error)) throw std::runtime_error(error);
        } else {
            const Vec3 forward = camera.Forward();
            const Mat4 view = LookAt(camera.position, camera.position + forward, {0, 1, 0});
            const Mat4 projection = Perspective(60.0f * kPi / 180.0f,
                static_cast<float>(windowWidth) / static_cast<float>(windowHeight), 0.01f, 1000.0f);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            renderScene(windowWidth, windowHeight, view, projection);
        }
        window.Swap();
    }
    glDeleteRenderbuffers(1, &xrDepthBuffer);
    glDeleteFramebuffers(1, &xrFramebuffer);
    glDeleteTextures(textures.size(), textures.data());
    glDeleteBuffers(1, &ebo); glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return Run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "tridigenerator_player: " << error.what() << '\n';
        return 1;
    }
}
