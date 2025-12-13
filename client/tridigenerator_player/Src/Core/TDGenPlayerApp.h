#pragma once
#include <GLES3/gl3.h>

#include "XrApp.h"

#include "EntityManager.h"

#include "../Systems/SceneSystem.h"
#include "../Systems/FrameLoaderSystem.h"
#include "../Systems/AudioSystem.h"
#include "../Systems/InputSystem.h"
#include "../Systems/TransformSystem.h"
#include "../Systems/RenderSystem.h"
#include "../Systems/UnlitGeometryRenderSystem.h"

//#include "input_actions.h"       // your XRInputActions module

class TDGenPlayerApp : public OVRFW::XrApp {
public:
    TDGenPlayerApp();
    virtual ~TDGenPlayerApp();

private:
    std::unique_ptr<EntityManager> entityManager_;

    std::unique_ptr<SceneSystem> sceneSystem_;
    std::unique_ptr<FrameLoaderSystem> frameLoaderSystem_;
    std::unique_ptr<AudioSystem> audioSystem_;
    std::unique_ptr<InputSystem> inputSystem_;
    std::unique_ptr<TransformSystem> transformSystem_;
    std::unique_ptr<RenderSystem> renderSystem_;
    std::unique_ptr<UnlitGeometryRenderSystem> unlitGeometryRenderSystem_;

    // XRInputActions xrInput_;   // action set instance (init in SessionInit)

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