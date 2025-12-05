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
#include "../Unsorted/WebmInMemoryDemuxer.h"

//#include "network_stream.h"
//#include "input_actions.h"       // your XRInputActions module

class TDGenPlayerApp : public OVRFW::XrApp {
public:
    TDGenPlayerApp();
    virtual ~TDGenPlayerApp();

private:
    EntityManager entityManager_;
    //Renderer renderer_;
    InputSystem input_;
    //AudioSystem audio_;
    //SceneManager scene_;
    std::unique_ptr<FrameLoader> frameLoader_;

    // Collection of all placed planes
    OVRFW::GeometryBuilder planeGeometry_;

    // Renderer of all the placed planes
    // gets reset from planeGeometry for any new plane
    OVRFW::UnlitGeometryRenderer planeRenderer_;

    // XRInputActions xrInput_;   // action set instance (init in SessionInit)

    // pointer to a frame inside the FrameLoader's pool.
    VideoFrame* currentFrame_ = nullptr;

    virtual std::vector<const char *> GetExtensions() override;
    virtual bool AppInit(const xrJava *context) override;
    virtual bool SessionInit() override;
    virtual void Update(const OVRFW::ovrApplFrameIn &in) override;
    virtual void AppRenderFrame(const OVRFW::ovrApplFrameIn& in, OVRFW::ovrRendererOutput& out) override;
    virtual void AppRenderEye(const OVRFW::ovrApplFrameIn& in, OVRFW::ovrRendererOutput& out, int eye) override;
    virtual void Render(const OVRFW::ovrApplFrameIn &in, OVRFW::ovrRendererOutput &out) override;
    virtual void SessionEnd() override;
    virtual void AppShutdown(const xrJava *context) override;
};