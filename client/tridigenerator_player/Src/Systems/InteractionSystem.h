#pragma once

#include "../Core/EntityManager.h"

class InteractionSystem {
public:
    bool Init(EntityManager& ecs);
    void Shutdown(EntityManager& ecs);
    void Update(EntityManager& ecs, float deltaSeconds);
    bool IsManipulating(EntityManager& ecs) const;
};
