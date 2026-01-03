#include <iostream>

#define LOG_TAG "SceneSystem"
#include "../Core/Logging.h"

#include "SceneSystem.h"

#include "../Systems/SceneSystem.h"

bool SceneSystem::Init(EntityManager& ecs) {
    ecs.ForEach<SceneComponent>(
            [&](EntityID e, SceneComponent &sC) {
        // Initially load the default scene
        LoadScene(sC.scene);
    });
    return true;
}

void SceneSystem::Shutdown(EntityManager& ecs) {
}

void SceneSystem::Update(EntityManager& ecs) {
}


void SceneSystem::LoadScene(const std::string& name) {
    if (name == "VR") {
        // Load VR scene resources

    } else if (name == "AR") {
        // Load AR scene resources

    } else {
        std::cout << "Unknown scene: " << name << std::endl;
    }
}
