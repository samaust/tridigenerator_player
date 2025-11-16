#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <GLES3/gl3.h>
#include <glm/glm.hpp>
#include <openxr/openxr.h>

// Meta/SampleXrFramework
#include "GUI/VRMenuObject.h"
#include "Input/ControllerRenderer.h"
#include "Input/HandRenderer.h"
#include "Input/TinyUI.h"
#include "Render/BitmapFont.h"
#include "Render/GeometryBuilder.h"
//#include "Render/GeometryRenderer.h"
#include "Render/GlGeometry.h"
#include "Render/GlProgram.h"
#include "Render/GlTexture.h"
#include "Render/SimpleBeamRenderer.h"
#include "Render/SurfaceRender.h"

// Meta/OVR
#include "OVR_Math.h"
#include "OVR_FileSys.h"

// Meta/meta_openxr_preview
#include "meta_openxr_preview/openxr_oculus_helpers.h"

#include "TDGenPlayerApp.h"

// All physical units in OpenXR are in meters, but sometimes it's more useful
// to think in cm, so this user defined literal converts from centimeters to meters
constexpr float operator"" _cm(long double centimeters)
{
    return static_cast<float>(centimeters * 0.01);
}
constexpr float operator"" _cm(unsigned long long centimeters)
{
    return static_cast<float>(centimeters * 0.01);
}

// For expressiveness; use _m rather than f literals when we mean meters
constexpr float operator"" _m(long double meters)
{
    return static_cast<float>(meters);
}
constexpr float operator"" _m(unsigned long long meters)
{
    return static_cast<float>(meters);
}

// helper to compile shader
static GLuint CompileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        // LOGE("Shader compile error: %s", log);
        glDeleteShader(s); return 0;
    }
    return s;
}
static GLuint LinkProgram(GLuint vert, GLuint frag) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vert);
    glAttachShader(p, frag);
    glLinkProgram(p);
    GLint ok=0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        //LOGE("Program link error: %s", log);
        glDeleteProgram(p); return 0;
    }
    return p;
}


TDGenPlayerApp::TDGenPlayerApp()
{
    BackgroundColor = OVR::Vector4f(0.55f, 0.35f, 0.1f, 1.0f);

    // Disable framework input management, letting this sample explicitly
    // call xrSyncActions() every frame; which includes control over which
    // ActionSet to set as active
    SkipInputHandling = false;
}

TDGenPlayerApp::~TDGenPlayerApp()
{
}

// Returns a list of OpenXR extensions requested for this app
// Note that the sample framework will filter out any extension
// that is not listed as supported.
std::vector<const char *> TDGenPlayerApp::GetExtensions()
{
    std::vector<const char *> extensions = XrApp::GetExtensions();
    extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    extensions.push_back(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME);
    return extensions;
}

// OVRFW::XrApp::Init() calls, among other things;
//  - xrInitializeLoaderKHR(...)
//  - xrCreateInstance with the extensions from GetExtensions(...),
//  - xrSuggestInteractionProfileBindings(...) to set up action bindings
// before calling the function below; AppInit():
bool TDGenPlayerApp::AppInit(const xrJava *context)
{
    OVRFW::XrApp::AppInit(context);

    // Load frame data
    //int height = 640;
    //int width = 640;
    frameloader = FrameLoader("http://192.168.111.250:8080");

    frameloader.loadManifest();
    gridHeight = frameloader.height;
    gridWidth = frameloader.width;

    frameloader.loadFrame(0);
    // use loader.getCurrentFrame().colorTex
    //     loader.getCurrentFrame().classTex
    // for rendering

    // Create GL program
    //CreateGLProgram();

    // create grid mesh (positions are placeholder; final positions set via textures or VBO update)

    mesh_.CreateGrid((size_t)gridWidth, (size_t)gridHeight);

    // create textures used to store colors and classification per-vertex
    //CreateTextures();

    // start network streamer (non-blocking thread). Replace URL/port placeholders.
    //net_.Start("PLACEHOLDER_WS_URL", "PLACEHOLDER_WS_PORT");

    // === Load shader ===
    const char* vShader =
            "#version 300 es\n"
            "uniform mat4 uView;\n"
            "uniform mat4 uProj;\n"
            "layout(location=0) in vec3 aPos;\n"
            "layout(location=1) in vec3 aNormal;\n"
            "out vec3 vNormal;\n"
            "void main(){ vNormal=aNormal; gl_Position=uProj*uView*vec4(aPos,1.0); }\n";

    const char* fShader =
            "#version 300 es\n"
            "precision mediump float;\n"
            "in vec3 vNormal;\n"
            "out vec4 fColor;\n"
            "void main(){ fColor=vec4(abs(vNormal),1.0); }\n";

    m_Program = OVRFW::GlProgram::Build(
            "",
            vShader,
            "",
            fShader,
            nullptr,
            0);
    uViewLoc = glGetUniformLocation(m_Program.Program, "uView");
    uProjLoc = glGetUniformLocation(m_Program.Program, "uProj");

    // === Create geometry ===
    //OVRFW::GeometryBuilder::SimpleGeo geoDef;
    //geoDef.GenerateCube();   // Placeholder geometry
    //m_Geometry = new OVRFW::Geometry(geoDef);

    OVRFW::GeometryBuilder gb;
    const OVR::Matrix4f capsuleMatrix = OVR::Matrix4f::Translation({0.00f, -0.015f, -0.01f}) *
                                   OVR::Matrix4f::RotationX(OVR::DegreeToRad(90.0f + 20.0f));

    // Note : could replace by a cube using BuildUnitCubeDescriptor
    gb.Add(
            OVRFW::BuildTesselatedCapsuleDescriptor(0.02f, 0.08f, 10, 7),
            0,
            {1.0f, 0.9f, 0.25f, 1.0f},
            capsuleMatrix);

    /// ring
    const OVR::Matrix4f ringMatrix = OVR::Matrix4f::Translation({0.0f, 0.02f, 0.04f});
    gb.Add(
            OVRFW::BuildTesselatedCylinderDescriptor(0.04f, 0.015f, 24, 2, 1.0f, 1.0f),
            0,
            {0.6f, 0.8f, 0.25f, 1.0f},
            ringMatrix);

    m_Geometry = gb.ToGeometry();

    glEnable(GL_DEPTH_TEST);

    return true;
}

// XrApp::InitSession() calls:
// - xrCreateSession(...) to create our Session
// - xrCreateReferenceSpace(...) for local and stage space
// - Create swapchain with xrCreateSwapchain(...)
// - xrAttachSessionActionSets(...)
// before calling SessionInit():
bool TDGenPlayerApp::SessionInit()
{
    // Initialize XRInputActions and create action spaces using XrApp helper functions
    //xrInput_.Init(GetInstance(), GetSession());
    //xrInput_.CreateActionSpaces(GetLocalSpace());

    return true;
}

// The update function is called every frame before the Render() function.
// Some of the key OpenXR function called by the framework prior to calling this function:
//  - xrPollEvent(...)
//  - xrWaitFrame(...)
void TDGenPlayerApp::Update(const OVRFW::ovrApplFrameIn &in)
{
    // if SkipInputHandling is True, we need to sync action sets ourselves
    // --- xrSyncAction
    //
    // Sync action sets (including default set if not skipped by SkipInputHandling)
    //OXR(xrSyncActions(Session, &syncInfo));

    // Application logic update here
    if (!frameloader.spawned)
    {
        frameloader.spawned = true;


        auto planeDescriptor = OVRFW::BuildTesselatedQuadDescriptor(
                frameloader.width-1,
                frameloader.height-1,
                true,
                false);
        OVR::Vector4f planeColor = {1.0f, 0.0f, 0.0f, 1.0f};
        planeGeometry_.Add(
            planeDescriptor,
            OVRFW::GeometryBuilder::kInvalidIndex,
            planeColor);

        auto d = planeGeometry_.ToGeometryDescriptor();
        d.attribs.position = frameloader.getCurrentFrame().position;
        d.attribs.color = frameloader.getCurrentFrame().color;

        planeRenderer_.Init(d);
        //planeRenderer_.SetPose(
        //    OVR::Posef(OVR::Quat<float>::Identity(), {0_m, 1.5_m, 0.0_m}));
        //planeRenderer_.SetScale({1.0f, 1.0f, 1.0f});
    }
    planeRenderer_.Update();
}

void TDGenPlayerApp::AppRenderFrame(const OVRFW::ovrApplFrameIn& in, OVRFW::ovrRendererOutput& out)
{
    OVRFW::XrApp::AppRenderFrame(in, out);
}

/*
 * Seems to be another version
void AppRenderFrame(const OVRFW::ovrApplFrameIn& in, OVRFW::ovrRendererOutput& out)
{
    for (int eye = 0; eye < in.NumViews; ++eye)
    {
        const OVRFW::OvrSceneView& view = in.SceneViews[eye];
        const OVR::Matrix4f projMatrix = OVRFW::GetProjectionMatrix(view);
        const OVR::Matrix4f viewMatrix = OVRFW::GetViewMatrix(view);

        glBindFramebuffer(GL_FRAMEBUFFER, view.Framebuffer->GetFrameBuffer());
        glViewport(0, 0, view.Framebuffer->GetWidth(), view.Framebuffer->GetHeight());
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(m_Program.Program);
        glUniformMatrix4fv(uViewLoc, 1, GL_FALSE, viewMatrix.M[0]);
        glUniformMatrix4fv(uProjLoc, 1, GL_FALSE, projMatrix.M[0]);

        m_Geometry->Bind();
        m_Geometry->Draw();
        m_Geometry->Unbind();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}
*/

void TDGenPlayerApp::AppRenderEye(const OVRFW::ovrApplFrameIn& in, OVRFW::ovrRendererOutput& out, int eye)
{
    OVRFW::XrApp::AppRenderEye(in, out, eye);

    /*// Setup GL state
    glUseProgram(program_);

    // Uniforms: model / view / proj are provided by framework's SceneView / FrameParams
    // Get view/proj from SceneView (XrApp helper)
    // TODO : use scene
    // OVRFW::OvrSceneView& scene = GetScene();
    // TODO : fix placeholders below to get actual matrices
    // scene.ViewMatrix and scene.ProjMatrix are available (OVRFW types).
    // Convert to float matrices as needed:
    //const float* viewM = scene.Viewpose.M[0]; // placeholder: adapt to real API if different
    //const float* projM = scene.Fov[eye].ProjectionMatrix.M[0]; // placeholder

    // Alternate: framework provides ovrApplFrameIn which often contains view/proj; adapt to your version.
    // For safety get matrices from SceneView:
    glm::mat4 view = glm::mat4(1.0f);
    glm::mat4 proj = glm::mat4(1.0f);
    // PLACEHOLDER: convert OVRFW matrices to glm::mat4 properly here.

    // We'll push model via uniform (identity or meshMatrix updated by grab controller)
    glm::mat4 model = glm::mat4(1.0f); // update by GrabController if used

    glUniformMatrix4fv(u_modelLoc, 1, GL_FALSE, &model[0][0]);
    glUniformMatrix4fv(u_viewLoc, 1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(u_projLoc, 1, GL_FALSE, &proj[0][0]);

    // bind textures
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, colorTex_);
    glUniform1i(u_colorTexLoc, 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, classTex_);
    glUniform1i(u_classTexLoc, 1);

    // If you have GPU depth texture from XR runtime, bind it here at unit 2. PLACEHOLDER:
    // glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, depthTex_); glUniform1i(u_depthTexLoc, 2);

    // Ambient light from runtime (if available). Placeholder ambient vector
    glm::vec3 ambient(1.0f, 1.0f, 1.0f);
    glUniform3f(u_ambientLoc, ambient.r, ambient.g, ambient.b);

    // Draw mesh
    mesh_.Bind();
    mesh_.Draw();
    mesh_.Unbind();

    // Unbind
    glUseProgram(0);*/
}

// Called by the XrApp framework after the Update function
void TDGenPlayerApp::Render(const OVRFW::ovrApplFrameIn &in, OVRFW::ovrRendererOutput &out)
{
    // Consume a frame from network (non-blocking). If available, update textures.
    //std::vector<uint8_t> frameBuf;
    // TODO: implement
    //if (net_.TryPopFrame(frameBuf)) {
    //    UpdateTexturesFromFrame(frameBuf);
    //}

    // The framework will call AppRenderEye for each eye.
    // Nothing else here.

    planeRenderer_.Render(out.Surfaces);
}

void TDGenPlayerApp::SessionEnd()
{
    //xrInput_.Destroy();
}

void TDGenPlayerApp::AppShutdown(const xrJava *context)
{
    //net_.Stop();
    mesh_.Destroy();
    if (program_) { glDeleteProgram(program_); program_ = 0; }
    if (colorTex_) glDeleteTextures(1, &colorTex_);
    if (classTex_) glDeleteTextures(1, &classTex_);
    if (posTex_) glDeleteTextures(1, &posTex_);

    //delete m_Geometry;
    //m_Geometry = nullptr;

    OVRFW::XrApp::AppShutdown(context);
}

std::string TDGenPlayerApp::LoadShaderSource(const std::string& filename) {
    // 1. Create the input file stream.
    std::ifstream file(filename);

    // 2. Error Check: Ensure the file was opened successfully.
    if (!file.is_open()) {
        std::cerr << "Error: Failed to open shader file " << filename << std::endl;
        return ""; // Return empty string on failure
    }

    // 3. Read the entire content into a std::string using iterators.
    // The first iterator points to the file's beginning.
    // The second (default-constructed) iterator acts as the stream's end.
    std::string source_code(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>()
    );

    return source_code;
}

void TDGenPlayerApp::CreateGLProgram() {
    // Load shader sources from assets or inline small shaders (use files in shaders/)

    // not implemented
    //std::string vertSrc = ReadTextFile("shaders/pointcloud_es.vert"); // implement ReadTextFile via FileSys
    //std::string fragSrc = ReadTextFile("shaders/pointcloud_es.frag");

    // TODO : fix path
    std::string vertSrc = LoadShaderSource("Shaders/pointcloud_es.vert");
    std::string fragSrc = LoadShaderSource("Shaders/pointcloud_es.frag");

    GLuint vs = CompileShader(GL_VERTEX_SHADER, vertSrc.c_str());
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragSrc.c_str());
    program_ = LinkProgram(vs, fs);
    glDeleteShader(vs); glDeleteShader(fs);

    u_modelLoc = glGetUniformLocation(program_, "u_model");
    u_viewLoc  = glGetUniformLocation(program_, "u_view");
    u_projLoc  = glGetUniformLocation(program_, "u_proj");
    u_ambientLoc = glGetUniformLocation(program_, "uAmbient");
    u_colorTexLoc = glGetUniformLocation(program_, "uColorTex");
    u_classTexLoc = glGetUniformLocation(program_, "uClassTex");
    u_depthTexLoc = glGetUniformLocation(program_, "uDepthTex");
}

void TDGenPlayerApp::CreateTextures() {
    // create color texture (RGB8)
    glGenTextures(1, &colorTex_);
    glBindTexture(GL_TEXTURE_2D, colorTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // allocate storage (will update via glTexSubImage2D)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, gridWidth, gridHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);

    // classification texture (R8)
    glGenTextures(1, &classTex_);
    glBindTexture(GL_TEXTURE_2D, classTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, gridWidth, gridHeight, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    // position texture optional (RGBA32F) if you sample position in vertex shader
    glGenTextures(1, &posTex_);
    glBindTexture(GL_TEXTURE_2D, posTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, gridWidth, gridHeight, 0, GL_RGBA, GL_FLOAT, nullptr);

    glBindTexture(GL_TEXTURE_2D, 0);
}

void TDGenPlayerApp::UpdateTexturesFromFrame(const std::vector<uint8_t>& frame) {
    // frame binary format: interleaved positions (float32 x3), colors (uint16/uint8?), classification
    // PLACEHOLDER: parse according to your server binary layout.
    // Example: assume color bytes stored as R,G,B per vertex contiguous: size = gridWidth*gridHeight*3
    const uint8_t* ptr = frame.data();
    size_t expectedColorBytes = gridWidth * gridHeight * 3;
    if (frame.size() >= expectedColorBytes) {
        glBindTexture(GL_TEXTURE_2D, colorTex_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, gridWidth, gridHeight, GL_RGB, GL_UNSIGNED_BYTE, ptr);
        ptr += expectedColorBytes;
    }
    // classification next
    size_t expectedClassBytes = gridWidth * gridHeight;
    if (frame.size() >= expectedColorBytes + expectedClassBytes) {
        glBindTexture(GL_TEXTURE_2D, classTex_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, gridWidth, gridHeight, GL_RED, GL_UNSIGNED_BYTE, ptr);
        ptr += expectedClassBytes;
    }

    // if positions present as floats
    size_t expectedPosBytes = gridWidth * gridHeight * sizeof(float) * 3;
    if (frame.size() >= expectedColorBytes + expectedClassBytes + expectedPosBytes) {
        glBindTexture(GL_TEXTURE_2D, posTex_);
        // we stored RGBA32F format, so need to expand xyz->rgba with w=1.0. If server supplies packed RGBA floats you can upload directly.
        // For performance, server might send separate textures; adapt here.
        ptr += 0; // placeholder
    }

    glBindTexture(GL_TEXTURE_2D, 0);
}