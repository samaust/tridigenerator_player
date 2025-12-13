#include "SceneSystem.h"
#include <iostream>

bool SceneSystem::Init(EntityManager& ecs) {
    // Initially load the default scene
    LoadScene(scene);
    return true;
}

void SceneSystem::Shutdown(EntityManager& ecs) {
}

void SceneSystem::Update(EntityManager& ecs) {
}


void SceneSystem::LoadScene(const std::string& name) {
}
