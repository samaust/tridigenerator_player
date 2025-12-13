#pragma once

#include "../Core/EntityManager.h"

class AudioSystem {
public:
    bool Init(EntityManager& ecs);
    void Shutdown(EntityManager& ecs);
    void Update(EntityManager& ecs);
};
