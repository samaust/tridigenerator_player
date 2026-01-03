#pragma once
#include <string>

#include "../Core/EntityManager.h"

#include "../Components/SceneComponent.h"

class SceneSystem {
public:
    bool Init(EntityManager& ecs);
    void Shutdown(EntityManager& ecs);
    void Update(EntityManager& ecs);

    void LoadScene(const std::string& name);
};
