#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "../Data/VipeDataset.h"
#include "../Data/MaskVisibilitySnapshot.h"

struct FrameLoaderComponent {
    std::string baseUrl;
    std::string dataDirectory;
    std::string manifestLocation;
    std::string videoLocation;
    std::string file;
    int width = 0;
    int height = 0;
    double fps = 16.0;
    float depthScaleFactor = 1.0f;
    VipeDataset dataset;
    VipeCatalog catalog;
    std::string selectedDatasetId;
    std::string errorMessage;
    std::atomic<bool> paused{false};

    std::atomic<bool> looping{true};
    std::atomic<bool> writerRunning{false};
    std::atomic<int> meshDetailDivisor{2};
    std::atomic<bool> dynamicIndexCullingEnabled{false};
    std::shared_ptr<MaskVisibilityPublisher> maskVisibilityPublisher =
        std::make_shared<MaskVisibilityPublisher>();

    // Default constructor
    FrameLoaderComponent() = default;

    // Delete Copy Constructor and Copy Assignment
    FrameLoaderComponent(const FrameLoaderComponent&) = delete;
    FrameLoaderComponent& operator=(const FrameLoaderComponent&) = delete;

    // Explicitly define the Move Constructor
    FrameLoaderComponent(FrameLoaderComponent&& other) noexcept
            : baseUrl(std::move(other.baseUrl)),
              dataDirectory(std::move(other.dataDirectory)),
              manifestLocation(std::move(other.manifestLocation)),
              videoLocation(std::move(other.videoLocation)),
            file(std::move(other.file)),
    width(other.width),
    height(other.height),
    fps(other.fps),
    depthScaleFactor(other.depthScaleFactor),
    dataset(std::move(other.dataset)),
    catalog(std::move(other.catalog)),
    selectedDatasetId(std::move(other.selectedDatasetId)),
    errorMessage(std::move(other.errorMessage))
    {
        paused.store(other.paused.load());
        // Manually move atomic values by loading from source and storing to destination
        looping.store(other.looping.load());
        writerRunning.store(other.writerRunning.load());
        meshDetailDivisor.store(other.meshDetailDivisor.load());
        dynamicIndexCullingEnabled.store(
            other.dynamicIndexCullingEnabled.load());
        maskVisibilityPublisher = std::move(other.maskVisibilityPublisher);
        if (!maskVisibilityPublisher) {
            maskVisibilityPublisher = std::make_shared<MaskVisibilityPublisher>();
        }
    }

    // Explicitly define the Move Assignment Operator
    FrameLoaderComponent& operator=(FrameLoaderComponent&& other) noexcept {
        if (this != &other) {
            baseUrl = std::move(other.baseUrl);
            dataDirectory = std::move(other.dataDirectory);
            manifestLocation = std::move(other.manifestLocation);
            videoLocation = std::move(other.videoLocation);
            file = std::move(other.file);
            width = other.width;
            height = other.height;
            fps = other.fps;
            depthScaleFactor = other.depthScaleFactor;
            dataset = std::move(other.dataset);
            catalog = std::move(other.catalog);
            selectedDatasetId = std::move(other.selectedDatasetId);
            errorMessage = std::move(other.errorMessage);
            paused.store(other.paused.load());
            // Manually move atomic values
            looping.store(other.looping.load());
            writerRunning.store(other.writerRunning.load());
            meshDetailDivisor.store(other.meshDetailDivisor.load());
            dynamicIndexCullingEnabled.store(
                other.dynamicIndexCullingEnabled.load());
            maskVisibilityPublisher = std::move(other.maskVisibilityPublisher);
            if (!maskVisibilityPublisher) {
                maskVisibilityPublisher =
                    std::make_shared<MaskVisibilityPublisher>();
            }
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
    swap(a.dataDirectory, b.dataDirectory);
    swap(a.manifestLocation, b.manifestLocation);
    swap(a.videoLocation, b.videoLocation);
    swap(a.file, b.file);
    swap(a.width, b.width);
    swap(a.height, b.height);
    swap(a.fps, b.fps); // This now works!
    swap(a.depthScaleFactor, b.depthScaleFactor);
    swap(a.dataset, b.dataset);
    swap(a.catalog, b.catalog);
    swap(a.selectedDatasetId, b.selectedDatasetId);
    swap(a.errorMessage, b.errorMessage);
    // Manually swap the atomic members by loading and storing their values
    bool pausedA = a.paused.load();
    bool pausedB = b.paused.load();
    a.paused.store(pausedB);
    b.paused.store(pausedA);

    bool loopingA = a.looping.load();
    bool loopingB = b.looping.load();
    a.looping.store(loopingB);
    b.looping.store(loopingA);

    bool runningA = a.writerRunning.load();
    bool runningB = b.writerRunning.load();
    a.writerRunning.store(runningB);
    b.writerRunning.store(runningA);

    int meshDetailA = a.meshDetailDivisor.load();
    int meshDetailB = b.meshDetailDivisor.load();
    a.meshDetailDivisor.store(meshDetailB);
    b.meshDetailDivisor.store(meshDetailA);

    bool cullingA = a.dynamicIndexCullingEnabled.load();
    bool cullingB = b.dynamicIndexCullingEnabled.load();
    a.dynamicIndexCullingEnabled.store(cullingB);
    b.dynamicIndexCullingEnabled.store(cullingA);
    swap(a.maskVisibilityPublisher, b.maskVisibilityPublisher);
}
