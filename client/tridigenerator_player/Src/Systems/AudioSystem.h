#pragma once

#include "../Core/EntityManager.h"
#include <atomic>

struct FrameLoaderState;

#if defined(ANDROID)
#include <aaudio/AAudio.h>
#endif

class AudioSystem {
public:
    bool Init(EntityManager& ecs);
    void Shutdown(EntityManager& ecs);
    void Update(EntityManager& ecs);

private:
#if defined(ANDROID)
    static aaudio_data_callback_result_t DataCallback(
        AAudioStream* stream, void* userData, void* audioData, int32_t numFrames);
    aaudio_data_callback_result_t Fill(float* output, int32_t numFrames);
    bool OpenStream();
    void CloseStream();

    AAudioStream* stream_ = nullptr;
    std::atomic<FrameLoaderState*> state_{nullptr};
    bool running_ = false;
#endif
};
