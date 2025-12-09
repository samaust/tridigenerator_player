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
#include "WebmInMemoryDemuxer.h"

// Simple frame info (from manifest)
struct FrameInfo {
    std::string file;
};

// Ring buffer slot
struct FrameSlot {
    // A pointer to a pre-allocated frame in the FrameLoader's framePool_.
    // The writer sets this pointer. The reader uses it.
    VideoFrame* frame = nullptr;

    // True if the writer has finished decoding a frame into this slot
    // and it is ready for the reader to consume.
    std::atomic<bool> ready{false};
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

    // Pass a frame by reference to be swapped with the next available frame.
    // The caller owns the frame, FrameLoader just fills it.
    bool SwapNextFrame(double nowSeconds, VideoFrame** outFramePtr);

    // Simple helpers
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
    float GetDepthScaleFactor() const { return depthScaleFactor; }

    void SetDepthScaleFactor(float factor) { depthScaleFactor = factor; }
    void SetFPS(int newFps);

    // Network fetch helper (provided earlier by you)
    bool HttpGetBinary(const std::string& url, std::vector<uint8_t>& out);

private:
    // Manifest / config
    std::string baseUrl;
    std::string file;
    int width = 0;
    int height = 0;
    std::atomic<int> fps{16};
    float depthScaleFactor = 1.0f;

    // A fixed pool of frame buffers
    std::vector<VideoFrame> framePool_; // Owns all the memory

    // Ring buffer
    std::vector<FrameSlot> ring; // size RING_SIZE
    std::atomic<int> writeIdx{0}; // index to write next
    std::atomic<int> readIdx{0};  // index of oldest unread slot

    // Manifest index pointers (which manifest entry to fetch next)
    std::atomic<int> manifestFetchIdx{0};
    std::atomic<bool> looping{true};

    // Writer thread
    std::thread writerThread;
    std::atomic<bool> writerRunning{false};

    // Synchronization for writer sleep / wake
    mutable std::mutex writerMutex;
    std::condition_variable writerCv;

    // Playback timing
    double nextReadTime = 0.0; // seconds (monotonic) when next frame should be consumed
    std::mutex timingMutex;    // protects nextReadTime updates

    // Parse a downloaded blob into VideoFrame (throws on error)
    static VideoFrame ParseFrame(const std::vector<uint8_t>& blob);

    // Load video by index (performs HttpGetBinary); returns VideoFrame
    std::vector<uint8_t> LoadVideoFromIndex(int idx);

    // Compute free slots in ring
    int ComputeFreeSlots() const;
};
