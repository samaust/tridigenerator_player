#pragma once
#include "XrApp.h"
#include "Render/GeometryBuilder.h"
//#include "Render/GeometryRenderer.h"
#include "Render/UnlitGeometryRenderer.h"

#include <GLES3/gl3.h>

#include "EntityManager.h"
#include "../Systems/Renderer.h"
#include "../Systems/Input.h"
#include "../Systems/Audio.h"
#include "../Systems/SceneManager.h"
#include "../Unsorted/FrameLoader.h"
#include "../Unsorted/gl_mesh.h"

//#include "network_stream.h"
//#include "input_actions.h"       // your XRInputActions module

class TDGenPlayerApp : public OVRFW::XrApp {
public:
    TDGenPlayerApp();
    virtual ~TDGenPlayerApp();

private:
    EntityManager entityManager;
    //Renderer renderer;
    InputSystem input;
    //AudioSystem audio;
    //SceneManager scene;
    FrameLoader frameloader;

    // Collection of all placed planes
    OVRFW::GeometryBuilder planeGeometry_;

    // Renderer of all the placed planes
    // gets reset from planeGeometry for any new plane
    OVRFW::UnlitGeometryRenderer planeRenderer_;

    GLuint program_ = 0;
    GLint u_modelLoc = -1;
    GLint u_viewLoc = -1;
    GLint u_projLoc = -1;
    GLint u_ambientLoc = -1;
    GLint u_colorTexLoc = -1;
    GLint u_classTexLoc = -1;
    GLint u_depthTexLoc = -1;

    GLMesh mesh_;              // helper contains VAO/VBO/IBO and update helpers
    // NetworkStream net_;        // background streamer, PLACEHOLDER for implementation
    // XRInputActions xrInput_;   // action set instance (init in SessionInit)

    int gridWidth = 256;       // set from manifest
    int gridHeight = 256;
    GLuint colorTex_ = 0;
    GLuint classTex_ = 0;
    GLuint posTex_ = 0;        // if you prefer storing positions in a texture
    //bool playing = true;
    //int frameIndex = 0;

    // OpenGL
    OVRFW::GlProgram m_Program;
    OVRFW::GlGeometry m_Geometry;
    GLint uViewLoc = -1;
    GLint uProjLoc = -1;

    virtual std::vector<const char *> GetExtensions() override;

    virtual bool AppInit(const xrJava *context) override;
    virtual bool SessionInit() override;
    virtual void Update(const OVRFW::ovrApplFrameIn &in) override;
    virtual void AppRenderFrame(const OVRFW::ovrApplFrameIn& in, OVRFW::ovrRendererOutput& out) override;
    virtual void AppRenderEye(const OVRFW::ovrApplFrameIn& in, OVRFW::ovrRendererOutput& out, int eye) override;
    virtual void Render(const OVRFW::ovrApplFrameIn &in, OVRFW::ovrRendererOutput &out) override;
    virtual void SessionEnd() override;
    virtual void AppShutdown(const xrJava *context) override;

    std::string LoadShaderSource(const std::string& filename);
    void CreateGLProgram();
    void CreateTextures();
    void UpdateTexturesFromFrame(const std::vector<uint8_t>& frame);
};