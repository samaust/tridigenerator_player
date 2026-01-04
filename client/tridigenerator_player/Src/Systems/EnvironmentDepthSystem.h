#pragma once

#include <openxr/openxr.h>

#include "FrameParams.h"

#include "../Core/EntityManager.h"

#include "../Components/CoreComponent.h"
#include "../States/CoreState.h"
#include "../States/EnvironmentDepthState.h"

class EnvironmentDepthSystem {
public:
    explicit EnvironmentDepthSystem(XrInstance instance);

    bool Init(EntityManager& ecs);
    void Shutdown(EntityManager& ecs);

    void SessionInit(EntityManager& ecs, XrSession session);
    void SessionEnd(EntityManager& ecs);

    void Update(EntityManager& ecs, const OVRFW::ovrApplFrameIn& in);

private:
    XrInstance instance_ = XR_NULL_HANDLE;

    void DestroyDepthResources(CoreState& cS, EnvironmentDepthState& edS);
};
