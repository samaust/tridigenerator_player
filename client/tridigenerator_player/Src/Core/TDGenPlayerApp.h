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
#include "../Components/ColorMatchingControl.h"
#include "../Components/ColorMatchingSettings.h"
#include "../Components/MeshDetailSettings.h"

namespace OVRFW {
class SimpleBeamRenderer;
class TinyUI;
class VRMenuObject;
}

//#include "input_actions.h"       // your XRInputActions module

class TDGenPlayerApp : public OVRFW::XrApp {
public:
    TDGenPlayerApp();
    virtual ~TDGenPlayerApp();

private:
    enum class UiMode {
        Datasets,
        Masks,
        ColorMatching,
        ColorMatchingSettings,
        MeshScale,
        MeshDetail
    };

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
    std::unique_ptr<OVRFW::TinyUI> playbackUi_;
    std::unique_ptr<OVRFW::SimpleBeamRenderer> pointerRenderer_;
    OVRFW::VRMenuObject* uiStatusLabel_ = nullptr;
    OVRFW::VRMenuObject* playbackButton_ = nullptr;
    OVRFW::VRMenuObject* meshScaleValueLabel_ = nullptr;
    OVRFW::VRMenuObject* meshScaleCurrentLabel_ = nullptr;
    std::array<bool, 256> maskToggleValues_{};
    UiMode pendingUiMode_ = UiMode::Datasets;
    UiMode currentUiMode_ = UiMode::Datasets;
    UiMode colorMatchingReturnMode_ = UiMode::Masks;
    UiMode meshScaleReturnMode_ = UiMode::Masks;
    UiMode meshDetailReturnMode_ = UiMode::Masks;
    bool uiRebuildPending_ = false;
    bool colorMatchingUiSnapshotValid_ = false;
    ColorMatchingTier colorMatchingUiRequested_ = ColorMatchingTier::Spatial;
    LightEstimateTier colorMatchingUiActive_ = LightEstimateTier::Unavailable;
    TierAvailability colorMatchingUiGlobal_ = TierAvailability::Checking;
    TierAvailability colorMatchingUiSpatial_ = TierAvailability::Checking;
    std::string colorMatchingUiAvailabilityMessage_;
    ColorMatchingSettings colorMatchingSaved_;
    ColorMatchingSettings colorMatchingDraft_;
    ColorMatchingSettings colorMatchingPreviewed_;
    std::string colorMatchingSettingsDatasetId_;
    std::string colorMatchingSettingsMessage_;
    bool colorMatchingEditActive_ = false;
    bool meshScaleLockUiValue_ = true;
    bool meshScaleUiLockedSnapshot_ = true;
    float meshScaleUiValueSnapshot_ = -1.0f;
    MeshDetailSettings meshDetailSaved_;
    MeshDetailSettings meshDetailDraft_;
    std::string meshDetailMessage_;
    bool meshDetailEditActive_ = false;
    bool uiVisible_ = true;
    bool uiAnchorInitialized_ = false;
    OVR::Posef uiAnchorPose_ = OVR::Posef::Identity();
    double lastUpdateSeconds_ = 0.0;
    EntityID objectEntity_ = 0;
    XrAction hapticAction_ = XR_NULL_HANDLE;

    void ShutdownUi();
    bool PrepareUi();
    void BuildPlaybackControls();
    void ShutdownPlaybackControls();
    void TogglePlayback();
    void RefreshPlaybackControls();
    void BuildDatasetPicker();
    void BuildMaskSelector();
    void BuildMeshScaleControls();
    void OpenMeshScaleControls(UiMode returnMode);
    void RefreshMeshScaleUi();
    void SetMeshScale(float scale);
    void StepMeshScale(int direction);
    void ResetMeshScale();
    void BuildMeshDetailControls();
    void OpenMeshDetailControls(UiMode returnMode);
    void PreviewMeshDetail(int divisor);
    void SaveMeshDetail();
    void CancelMeshDetailEdits();
    void LoadMeshDetailSettings();
    int ReadMeshDetailDivisor();
    bool StoreMeshDetailDivisor(int divisor);
    void BuildColorMatchingControls();
    void BuildColorMatchingSettingsControls();
    void OpenColorMatchingControls(UiMode returnMode);
    void SelectColorMatchingTier(ColorMatchingTier tier);
    void PreviewColorMatchingDraft();
    void SaveColorMatchingDraft();
    void ResetColorMatchingDraft();
    void CancelColorMatchingEdits();
    void LoadColorMatchingSettingsForDataset();
    bool StoreColorMatchingSettings(const std::string& datasetId, const std::string& json);
    std::string ReadColorMatchingSettings(const std::string& datasetId);
    void RefreshColorMatchingUi();
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
