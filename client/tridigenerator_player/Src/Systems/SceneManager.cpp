#include "SceneManager.h"
#include <iostream>

void SceneManager::LoadScene(const std::string& name) {
    std::cout << "Loading scene: " << name << "\n";
}

void SceneManager::Update(EntityManager&) {
    std::cout << "Updating scene...\n";
}
