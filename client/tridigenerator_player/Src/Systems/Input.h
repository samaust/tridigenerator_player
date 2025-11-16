#pragma once

class InputSystem {
public:
    void Update();
    bool ShouldQuit() const { return quit; }

private:
    bool quit = false;
};
