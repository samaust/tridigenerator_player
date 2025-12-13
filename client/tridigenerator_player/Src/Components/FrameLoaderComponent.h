#pragma once

#include <atomic>
#include <string>
#include <utility>

struct FrameLoaderComponent {
    std::string baseUrl;
    std::string file;
    int width = 0;
    int height = 0;
    int fps = 16;
    float fovX_deg = 75.0f;
    float depthScaleFactor = 1.0f;

    std::atomic<bool> looping{true};
    std::atomic<bool> writerRunning{false};

    // Default constructor
    FrameLoaderComponent() = default;

    // Delete Copy Constructor and Copy Assignment
    FrameLoaderComponent(const FrameLoaderComponent&) = delete;
    FrameLoaderComponent& operator=(const FrameLoaderComponent&) = delete;

    // Explicitly define the Move Constructor
    FrameLoaderComponent(FrameLoaderComponent&& other) noexcept
            : baseUrl(std::move(other.baseUrl)),
            file(std::move(other.file)),
    width(other.width),
    height(other.height),
    fps(other.fps),
    depthScaleFactor(other.depthScaleFactor)
    {
        // Manually move atomic values by loading from source and storing to destination
        looping.store(other.looping.load());
        writerRunning.store(other.writerRunning.load());
    }

    // Explicitly define the Move Assignment Operator
    FrameLoaderComponent& operator=(FrameLoaderComponent&& other) noexcept {
        if (this != &other) {
            baseUrl = std::move(other.baseUrl);
            file = std::move(other.file);
            width = other.width;
            height = other.height;
            fps = other.fps;
            depthScaleFactor = other.depthScaleFactor;
            // Manually move atomic values
            looping.store(other.looping.load());
            writerRunning.store(other.writerRunning.load());
        }
        return *this;
    }
};

// Now, we still need a custom swap because of the remaining atomics.
// But it's much simpler now.
inline void swap(FrameLoaderComponent& a, FrameLoaderComponent& b) noexcept {
    using std::swap; // Enable ADL for standard types

    // Swap all the non-atomic members directly
    swap(a.baseUrl, b.baseUrl);
    swap(a.file, b.file);
    swap(a.width, b.width);
    swap(a.height, b.height);
    swap(a.fps, b.fps); // This now works!
    swap(a.depthScaleFactor, b.depthScaleFactor);

    // Manually swap the atomic members by loading and storing their values
    bool loopingA = a.looping.load();
    bool loopingB = b.looping.load();
    a.looping.store(loopingB);
    b.looping.store(loopingA);

    bool runningA = a.writerRunning.load();
    bool runningB = b.writerRunning.load();
    a.writerRunning.store(runningB);
    b.writerRunning.store(runningA);
}