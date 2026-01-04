#pragma once

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "../Core/EntityManager.h"

#include "../Components/CoreComponent.h"
#include "../States/CoreState.h"

class CoreSystem {
public:
    CoreSystem(XrInstance instance, XrSystemId systemId);
    bool Init(EntityManager& ecs);
    std::vector<const char*> GetExtensions();
    void Shutdown(EntityManager& ecs);
    void Update(EntityManager& ecs);
    void SessionInit(EntityManager& ecs, XrSession session);
    void SessionEnd(EntityManager& ecs);
    void SetLocalSpace(EntityManager& ecs, XrSpace localSpace);
    bool BuildPassthroughLayer(
        EntityManager& ecs,
        XrCompositionLayerPassthroughFB& outLayer,
        XrSpace space);

private:
    void InitHandtracking(CoreComponent& cC, CoreState& cS);
    void InitPassthrough(CoreComponent& cC, CoreState& cS);
    bool ExtensionsArePresent(const std::vector<const char*>& extensionList) const;
    std::vector<XrExtensionProperties> GetXrExtensionProperties() const;
    std::vector<const char*> PassthroughRequiredExtensionNames();
    std::vector<const char*> DepthRequiredExtensionNames();
    XrPassthroughLayerFB CreateProjectedLayer(CoreState& cS) const;
    void DestroyLayer(CoreState& cS) const;
    void PassthroughStart(CoreState& cS) const;
    void PassthroughPause(CoreState& cS) const;

    XrInstance instance_;
    XrSystemId systemId_;
    XrSpace localSpace_ = XR_NULL_HANDLE;
};
