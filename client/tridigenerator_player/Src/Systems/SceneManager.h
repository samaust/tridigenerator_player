#pragma once
#include "../Core/EntityManager.h"
#include <string>

class SceneManager {
public:
    void LoadScene(const std::string& name);
    void Update(EntityManager& ecs);
};
