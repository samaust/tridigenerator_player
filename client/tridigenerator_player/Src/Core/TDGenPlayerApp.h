#pragma once
#include <array>

#include <GLES3/gl3.h>

#include "XrApp.h"

#include "EntityManager.h"

#include "../Systems/CoreSystem.h"
#include "../Systems/SceneSystem.h"
#include "../Systems/FrameLoaderSystem.h"
#include "../Systems/AudioSystem.h"
#include "../Systems/InputSystem.h"
#include "../Systems/InteractionSystem.h"
#include "../Systems/TransformSystem.h"
#include "../Systems/RenderSystem.h"
#include "../Systems/EnvironmentDepthSystem.h"
#include "../Systems/CameraLightEstimationSystem.h"
#include "../Systems/UnlitGeometryRenderSystem.h"
#include "../States/InteractionState.h"

namespace OVRFW {
class TinyUI;
class VRMenuObject;
}

//#include "input_actions.h"       // your XRInputActions module

class TDGenPlayerApp : public OVRFW::XrApp {
public:
    TDGenPlayerApp();
    virtual ~TDGenPlayerApp();

private:
    enum class UiMode { Datasets, Masks };

    std::unique_ptr<EntityManager> entityManager_;

    std::unique_ptr<CoreSystem> coreSystem_;
    std::unique_ptr<SceneSystem> sceneSystem_;
    std::unique_ptr<FrameLoaderSystem> frameLoaderSystem_;
    std::unique_ptr<AudioSystem> audioSystem_;
    std::unique_ptr<InputSystem> inputSystem_;
    std::unique_ptr<InteractionSystem> interactionSystem_;
    std::unique_ptr<TransformSystem> transformSystem_;
    std::unique_ptr<RenderSystem> renderSystem_;
    std::unique_ptr<EnvironmentDepthSystem> environmentDepthSystem_;
    std::unique_ptr<CameraLightEstimationSystem> cameraLightEstimationSystem_;
    std::unique_ptr<UnlitGeometryRenderSystem> unlitGeometryRenderSystem_;
    std::unique_ptr<OVRFW::TinyUI> ui_;
    OVRFW::VRMenuObject* uiStatusLabel_ = nullptr;
    std::array<bool, 256> maskToggleValues_{};
    UiMode pendingUiMode_ = UiMode::Datasets;
    bool uiRebuildPending_ = false;
    bool uiVisible_ = true;
    double lastUpdateSeconds_ = 0.0;
    EntityID objectEntity_ = 0;
    XrAction hapticAction_ = XR_NULL_HANDLE;

    void ShutdownUi();
    void BuildDatasetPicker();
    void BuildMaskSelector();
    void RequestUiMode(UiMode mode);
    void SelectDataset(const std::string& datasetId);
    void DispatchHaptic(HapticEvent event, uint8_t controllerMask);
    void StopHaptics();

    // XRInputActions xrInput_;   // action set instance (init in SessionInit)

    virtual std::vector<const char *> GetExtensions() override;
    virtual std::unordered_map<XrPath, std::vector<XrActionSuggestedBinding>>
        GetSuggestedBindings(XrInstance instance) override;
    virtual bool AppInit(const xrJava *context) override;
    virtual bool SessionInit() override;
    virtual void Update(const OVRFW::ovrApplFrameIn &in) override;
    virtual void AppRenderFrame(const OVRFW::ovrApplFrameIn& in, OVRFW::ovrRendererOutput& out) override;
    virtual void AppRenderEye(const OVRFW::ovrApplFrameIn& in, OVRFW::ovrRendererOutput& out, int eye) override;
    virtual void Render(const OVRFW::ovrApplFrameIn &in, OVRFW::ovrRendererOutput &out) override;
    virtual void SessionEnd() override;
    virtual void AppShutdown(const xrJava *context) override;
    virtual void PreProjectionAddLayer(xrCompositorLayerUnion* layers, int& layerCount) override;
};
