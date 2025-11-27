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
#include "Dav1d/Dav1dPlayer.h"

struct FrameData {
    int width = 0;
    int height = 0;
    int pointCount = 0;
    const uint8_t* textureYData = nullptr;
    const uint8_t* textureUData = nullptr;
    const uint8_t* textureVData = nullptr;
    int strideY = 0;
    int strideU = 0;
    int strideV = 0;

    // constructor
    FrameData(int w, int h, int pc, const uint8_t* dY, const uint8_t* dU, const uint8_t* dV, const int sY, const int sU, const int sV)
            : width(w), height(h), pointCount(pc), textureYData(dY), textureUData(dU), textureVData(dV), strideY(sY), strideU(sU), strideV(sV) {}

    // default constructor
    FrameData() = default;
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
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
    void SetFPS(int newFps);

    // Network fetch helper (provided earlier by you)
    bool HttpGetBinary(const std::string& url, std::vector<uint8_t>& out);

private:
    Dav1dPlayer player;

    // Parse a downloaded blob into FrameData (throws on error)
    static FrameData ParseFrame(const std::vector<uint8_t>& blob);

    // Load remote frame by index (performs HttpGetBinary + ParseFrame); returns FrameData
    std::vector<uint8_t> LoadVideoFromIndex(int idx);

    // Compute free slots in ring
    int ComputeFreeSlots() const;

private:
    // Manifest / config
    std::string baseUrl;

    std::string file;
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

