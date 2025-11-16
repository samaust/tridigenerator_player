#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "OVR_Math.h"

// Simple frame info (from manifest)
struct FrameInfo {
    std::string file;
};

struct FrameData {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pointCount = 0;

    std::vector<OVR::Vector3f> positions; // XYZ
    std::vector<OVR::Vector4f> colors;    // RGBA
};

// Ring buffer slot
struct FrameSlot {
    FrameData data;
    std::atomic<bool> ready{false};

    // Default constructor
    FrameSlot() : data(), ready(false) {}

    // Custom move constructor
    FrameSlot(FrameSlot&& other) noexcept :
            data(std::move(other.data)),
            ready(other.ready.load()) // Atomically load the value from the old atomic
    {}

    // Add a move assignment operator
    FrameSlot& operator=(FrameSlot&& other) noexcept {
        if (this != &other) {
            data = std::move(other.data);
            ready.store(other.ready.load()); // Atomically load and store
        }
        return *this;
    }

    // Since we've defined move operations, the copy operations are deleted.
    // That's fine, as we don't need to copy FrameSlots.
    FrameSlot(const FrameSlot&) = delete;
    FrameSlot& operator=(const FrameSlot&) = delete;
};

static constexpr int RING_SIZE = 8;

class FrameLoader {
public:
    FrameLoader() : baseUrl("") {}
    explicit FrameLoader(const std::string& baseUrl_);
    ~FrameLoader();

    // Initialization
    bool LoadManifest();

    // Start/stop background writer
    void StartBackgroundWriter();
    void StopBackgroundWriter();

    // Called by writer thread (internal)
    void WriterLoop();

    // Non-blocking call from Update(): will advance playback when it's time.
    // nowSeconds should be high-resolution monotonic time in seconds (double).
    // Returns true when currentFrame has been updated (a new frame was consumed).
    bool ReadFrameIfNeeded(double nowSeconds);

    // Returns a const reference to the most recently consumed frame (owned by loader).
    // Valid until the next successful ReadFrameIfNeeded (or until Stop/Shutdown).
    const FrameData& GetCurrentFrame() const { return currentFrame; }

    // Simple helpers
    int GetNumFrames() const { return (int)frames.size(); }
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
    void SetFPS(int newFps);

    // Network fetch helper (provided earlier by you)
    bool HttpGetBinary(const std::string& url, std::vector<uint8_t>& out);

private:
    // Parse a downloaded blob into FrameData (throws on error)
    static FrameData ParseFrame(const std::vector<uint8_t>& blob);

    // Load remote frame by index (performs HttpGetBinary + ParseFrame); returns FrameData
    FrameData LoadFrameFromIndex(int idx);

    // Compute free slots in ring
    int ComputeFreeSlots() const;

private:
    // Manifest / config
    std::string baseUrl;
    std::vector<FrameInfo> frames;
    int width = 0;
    int height = 0;
    std::atomic<int> fps{16};

    // Ring buffer
    std::vector<FrameSlot> ring; // size RING_SIZE
    std::atomic<int> writeIdx{0}; // index to write next
    std::atomic<int> readIdx{0};  // index of oldest unread slot

    // Manifest index pointers (which manifest entry to fetch next)
    std::atomic<int> manifestFetchIdx{0};
    std::atomic<bool> looping{true};

    // Current playback frame (owned by loader; moved from slot on read)
    FrameData currentFrame;

    // Writer thread
    std::thread writerThread;
    std::atomic<bool> writerRunning{false};

    // Synchronization for writer sleep / wake
    mutable std::mutex writerMutex;
    std::condition_variable writerCv;

    // Playback timing
    double nextReadTime = 0.0; // seconds (monotonic) when next frame should be consumed
    std::mutex timingMutex;    // protects nextReadTime updates
};
