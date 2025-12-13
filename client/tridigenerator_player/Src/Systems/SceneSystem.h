#pragma once
#include "../Core/EntityManager.h"
#include <string>

class SceneSystem {
public:
    bool Init(EntityManager& ecs);
    void Shutdown(EntityManager& ecs);
    void Update(EntityManager& ecs);

    void LoadScene(const std::string& name);
};
