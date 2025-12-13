#pragma once

#include "OVR_Math.h"

#include "../Core/EntityManager.h"

#include "../Components/TransformComponent.h"

#include "../States/TransformState.h"

class TransformSystem {
public:
    static void SetPose(TransformComponent &tC,
                        TransformState &tS,
                        OVR::Posef newPose);
    static void SetScale(TransformComponent &tC,
                         TransformState &tS,
                         OVR::Vector3f newScale);

    bool Init(EntityManager& ecs);
    void Shutdown(EntityManager& ecs);
    void Update(EntityManager& ecs);
private:
    // Do not call directly, use SetPosition/SetScale instead
    static void SetModelMatrix(TransformComponent &tC, TransformState &tS);
};
