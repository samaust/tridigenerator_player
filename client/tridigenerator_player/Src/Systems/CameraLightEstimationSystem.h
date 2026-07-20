#pragma once

#include <openxr/openxr.h>

#include "../Core/EntityManager.h"
#include "../States/CameraLightEstimationState.h"

namespace OVRFW { struct ovrApplFrameIn; }

class CameraLightEstimationSystem {
public:
    explicit CameraLightEstimationSystem(XrInstance instance);
    bool Init(EntityManager& ecs);
    void SessionInit(EntityManager& ecs, XrSession session);
    void Update(EntityManager& ecs, const OVRFW::ovrApplFrameIn& in, bool focused);
    void SessionEnd(EntityManager& ecs);
    void Shutdown(EntityManager& ecs);

private:
    XrInstance instance_ = XR_NULL_HANDLE;
};
