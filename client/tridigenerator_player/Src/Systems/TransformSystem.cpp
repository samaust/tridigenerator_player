#define LOG_TAG "TransformSystem"
#include "../Core/Logging.h"

#include "TransformSystem.h"

bool operator!=(const OVR::Posef& a, const OVR::Posef& b) {
    return (a.Rotation != b.Rotation) || (a.Translation != b.Translation);
}

// It's good practice to also define operator==
bool operator==(const OVR::Posef& a, const OVR::Posef& b) {
    return (a.Rotation == b.Rotation) && (a.Translation == b.Translation);
}

void TransformSystem::SetPose(TransformComponent &tC,
                              TransformState &tS,
                              OVR::Posef newPose) {
    if (tC.modelPose != newPose) { // Check for actual change
        tC.modelPose = newPose;
        SetModelMatrix(tC, tS);
    }
}

void TransformSystem::SetScale(TransformComponent &tC,
                               TransformState &tS,
                               OVR::Vector3f newScale) {
    if (tC.modelScale != newScale) { // Check for actual change
        tC.modelScale = newScale;
        SetModelMatrix(tC, tS);
    }
}

// Do not call directly, use SetPosition/SetScale instead
void TransformSystem::SetModelMatrix(TransformComponent &tC,
                                     TransformState &tS) {
    tC.modelPose.Rotation.Normalize();
    tS.modelMatrix = OVR::Matrix4f(tC.modelPose) * OVR::Matrix4f::Scaling(tC.modelScale);
}

bool TransformSystem::Init(EntityManager& ecs) {
    return true;
}

void TransformSystem::Shutdown(EntityManager& ecs) {
}

void TransformSystem::Update(EntityManager& ecs) {

}
