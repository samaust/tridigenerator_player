#pragma once

#include <openxr/openxr.h>

#include "FrameParams.h"
#include "Render/SurfaceRender.h"

#include "../Core/EntityManager.h"

class InputSystem {
public:
    bool Init(EntityManager& ecs);
    void Shutdown(EntityManager& ecs);
    bool SessionInit(EntityManager& ecs, XrSession session);
    void SessionEnd(EntityManager& ecs);
    void Update(EntityManager& ecs, const OVRFW::ovrApplFrameIn& in);
    void Render(EntityManager& ecs, std::vector<OVRFW::ovrDrawSurface>& surfaces);
};
