#include "AudioSystem.h"

#include <algorithm>
#include <cstring>

#define LOG_TAG "AudioSystem"
#include "../Core/Logging.h"

#include "../Components/FrameLoaderComponent.h"
#include "../States/FrameLoaderState.h"

bool AudioSystem::Init(EntityManager& ecs) {
#if defined(ANDROID)
    (void)ecs;
    if (!OpenStream()) {
        LOGE("AAudio initialization failed; video will continue silently");
    }
#else
    (void)ecs;
#endif
    return true;
}

void AudioSystem::Shutdown(EntityManager& ecs) {
#if defined(ANDROID)
    (void)ecs;
    CloseStream();
#else
    (void)ecs;
#endif
}

void AudioSystem::Update(EntityManager& ecs) {
#if defined(ANDROID)
    FrameLoaderComponent* component = nullptr;
    FrameLoaderState* state = nullptr;
    ecs.ForEachMulti<FrameLoaderComponent, FrameLoaderState>(
        [&](EntityID, FrameLoaderComponent& candidate, FrameLoaderState& candidateState) {
            if (!component) {
                component = &candidate;
                state = &candidateState;
            }
        });
    state_.store(state, std::memory_order_release);
    if (!stream_ || !component || !state) return;

    bool hasQueuedAudio = false;
    {
        std::lock_guard<std::mutex> lock(state->audioMutex);
        hasQueuedAudio = !state->audioQueue.empty();
    }
    const bool shouldRun =
        state->audioAvailable.load(std::memory_order_acquire) &&
        hasQueuedAudio && !component->paused;
    if (shouldRun && !running_) {
        if (AAudioStream_requestStart(stream_) == AAUDIO_OK) {
            running_ = true;
            state->audioStarted.store(true, std::memory_order_release);
        } else {
            LOGE("Could not start AAudio stream; continuing silently");
            state->audioAvailable.store(false, std::memory_order_release);
        }
    } else if (!shouldRun && running_) {
        AAudioStream_requestPause(stream_);
        running_ = false;
        state->audioStarted.store(false, std::memory_order_release);
    }
#else
    (void)ecs;
#endif
}

#if defined(ANDROID)
bool AudioSystem::OpenStream() {
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, 48000);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, &AudioSystem::DataCallback, this);
    const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || !stream_) {
        stream_ = nullptr;
        return false;
    }
    if (AAudioStream_getSampleRate(stream_) != 48000 ||
        AAudioStream_getChannelCount(stream_) != 2 ||
        AAudioStream_getFormat(stream_) != AAUDIO_FORMAT_PCM_FLOAT) {
        LOGE("AAudio did not provide the requested 48 kHz float stereo format");
        CloseStream();
        return false;
    }
    return true;
}

void AudioSystem::CloseStream() {
    if (!stream_) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
    state_.store(nullptr, std::memory_order_release);
    running_ = false;
}

aaudio_data_callback_result_t AudioSystem::DataCallback(
        AAudioStream*, void* userData, void* audioData, int32_t numFrames) {
    return static_cast<AudioSystem*>(userData)->Fill(
        static_cast<float*>(audioData), numFrames);
}

aaudio_data_callback_result_t AudioSystem::Fill(float* output, int32_t numFrames) {
    const size_t requestedSamples = static_cast<size_t>(numFrames) * 2;
    std::fill(output, output + requestedSamples, 0.0f);
    FrameLoaderState* state = state_.load(std::memory_order_acquire);
    if (!state || !state->audioStarted.load(std::memory_order_acquire)) {
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    size_t written = 0;
    std::unique_lock<std::mutex> lock(state->audioMutex, std::try_to_lock);
    if (lock.owns_lock()) {
        while (written < requestedSamples && !state->audioQueue.empty()) {
            AudioPcmBlock& block = state->audioQueue.front();
            const size_t available = block.samples.size() - state->audioSampleOffset;
            const size_t count = std::min(available, requestedSamples - written);
            std::memcpy(
                output + written,
                block.samples.data() + state->audioSampleOffset,
                count * sizeof(float));
            written += count;
            state->audioSampleOffset += count;
            if (state->audioSampleOffset == block.samples.size()) {
                state->audioQueue.pop_front();
                state->audioSampleOffset = 0;
            }
        }
    }
    state->audioPlayedFrames.fetch_add(numFrames, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
#endif
