#pragma once

#include "../Core/EntityManager.h"

#include "../Components/FrameLoaderComponent.h"
#include "../States/FrameLoaderState.h"

class FrameLoaderSystem {
public:
    bool Init(EntityManager& ecs);
    void Shutdown(EntityManager& ecs);
    void Update(EntityManager& ecs, double nowSeconds);
    void StopBackgroundWriter(FrameLoaderComponent& flC,
                              FrameLoaderState& flS);
    void SetFPS(int newFps, FrameLoaderComponent& flC, FrameLoaderState& flS);
private:
    static size_t writeString(void* ptr, size_t size, size_t nmemb, void* userdata);
    static size_t writeBinary(void* ptr, size_t size, size_t nmemb, void* userdata);
    bool LoadManifest(FrameLoaderComponent& flC,
                      FrameLoaderState& flS);
    void StartBackgroundWriter(FrameLoaderComponent& flC,
                               FrameLoaderState& flS);
    void WriterLoop(FrameLoaderComponent& flC, FrameLoaderState& flS);
    std::vector<uint8_t> LoadVideoFromUrl(std::string& baseUrl, std::string file);
    bool HttpGetBinary(const std::string& url, std::vector<uint8_t>& out);
    int ComputeFreeSlots(FrameLoaderState& flS) const;
    bool SwapNextFrame(
        double nowSeconds,
        FrameLoaderComponent& flC,
        FrameLoaderState& flS);
};
