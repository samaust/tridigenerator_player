#pragma once

#include <openxr/openxr.h>

#include "../Core/EntityManager.h"
#include "../Data/GpuTimingManager.h"
#include "../Data/PerformanceTimingStats.h"
#include "../States/CameraLightEstimationState.h"

namespace OVRFW { struct ovrApplFrameIn; }

class CameraLightEstimationSystem {
public:
    explicit CameraLightEstimationSystem(
        XrInstance instance,
        std::shared_ptr<PerformanceTimingStats> performanceTiming = nullptr,
        GpuTimingManager* gpuTiming = nullptr);
    bool Init(EntityManager& ecs);
    void SessionInit(EntityManager& ecs, XrSession session);
    void Update(EntityManager& ecs, const OVRFW::ovrApplFrameIn& in, bool focused);
    void SessionEnd(EntityManager& ecs);
    void Shutdown(EntityManager& ecs);

private:
    XrInstance instance_ = XR_NULL_HANDLE;
    std::shared_ptr<PerformanceTimingStats> performanceTiming_;
    GpuTimingManager* gpuTiming_ = nullptr;
};
