#pragma once

#include <thread>
#include <utility> // For std::swap and std::move
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>

#include "Render/VideoFrame.h"
#include "../Videos/AudioPcmBlock.h"

// Ring buffer slot
struct FrameSlot {
    // A pointer to a pre-allocated frame in the FrameLoader's framePool_.
    // The writer sets this pointer. The reader uses it.
    VideoFrame* frame = nullptr;

    // True if the writer has finished decoding a frame into this slot
    // and it is ready for the reader to consume.
    std::atomic<bool> ready{false};

    // 1. Default constructor
    FrameSlot() = default;

    // 2. Delete Copy Operations
    FrameSlot(const FrameSlot&) = delete;
    FrameSlot& operator=(const FrameSlot&) = delete;

    // 3. Define the Move Constructor
    FrameSlot(FrameSlot&& other) noexcept
            : frame(other.frame)
            {
                    // Manually move the atomic's value
                    ready.store(other.ready.load());
            other.frame = nullptr; // Leave source in a valid state
            }

    // 4. Define the Move Assignment Operator
    FrameSlot& operator=(FrameSlot&& other) noexcept {
        if (this != &other) {
            frame = other.frame;
            ready.store(other.ready.load());
            other.frame = nullptr; // Leave source in a valid state
        }
        return *this;
    }
};

struct FrameLoaderState {
    // A fixed pool of frame buffers
    std::vector<VideoFrame> framePool; // Owns all the memory

    // Ring buffer
    std::vector<FrameSlot> ring; // size RING_SIZE
    std::atomic<int> writeIdx{0}; // index to write next
    std::atomic<int> readIdx{0};  // index of oldest unread slot

    // Writer thread
    std::thread writerThread;

    // Synchronization for writer sleep / wake
    mutable std::mutex writerMutex;
    std::condition_variable writerCv;

    // Playback timing
    double nextReadTime = 0.0; // seconds (monotonic) when next frame should be consumed
    std::mutex timingMutex;    // protects nextReadTime updates

    // Optional decoded stereo audio. The writer fills this queue and the
    // Android AAudio callback consumes it.
    std::deque<AudioPcmBlock> audioQueue;
    std::mutex audioMutex;
    size_t audioSampleOffset = 0;
    std::atomic<bool> audioAvailable{false};
    std::atomic<bool> audioStarted{false};
    std::atomic<int64_t> audioPlayedFrames{0};
    std::atomic<int64_t> audioEpochUs{0};
    std::atomic<int> audioSampleRate{48000};

    VideoFrame** framePtr = nullptr;
    std::atomic<bool> frameReady{false};

    // Default constructor
    FrameLoaderState() = default;

    // Delete Copy Constructor and Copy Assignment
    FrameLoaderState(const FrameLoaderState&) = delete;
    FrameLoaderState& operator=(const FrameLoaderState&) = delete;

    // Explicitly define the Move Constructor
    FrameLoaderState(FrameLoaderState&& other) noexcept
            : framePool(std::move(other.framePool)),
            ring(std::move(other.ring)),
    writerThread(std::move(other.writerThread)),
    nextReadTime(other.nextReadTime),
    audioQueue(std::move(other.audioQueue)),
    audioSampleOffset(other.audioSampleOffset),
    framePtr(other.framePtr) {
        // Manually move atomic values
        writeIdx.store(other.writeIdx.load());
        readIdx.store(other.readIdx.load());
        audioAvailable.store(other.audioAvailable.load());
        audioStarted.store(other.audioStarted.load());
        audioPlayedFrames.store(other.audioPlayedFrames.load());
        audioEpochUs.store(other.audioEpochUs.load());
        audioSampleRate.store(other.audioSampleRate.load());
        // Mutexes and CVs are not moved; they are default-constructed in the new object.
        // `other` is now in a valid but unspecified state.
        other.framePtr = nullptr;
    }

    // Explicitly define the Move Assignment Operator
    FrameLoaderState& operator=(FrameLoaderState&& other) noexcept {
        if (this != &other) {
            // Move movable members
            framePool = std::move(other.framePool);
            ring = std::move(other.ring);
            writerThread = std::move(other.writerThread);
            nextReadTime = other.nextReadTime;
            audioQueue = std::move(other.audioQueue);
            audioSampleOffset = other.audioSampleOffset;
            framePtr = other.framePtr;

            // Manually move atomic values
            writeIdx.store(other.writeIdx.load());
            readIdx.store(other.readIdx.load());
            audioAvailable.store(other.audioAvailable.load());
            audioStarted.store(other.audioStarted.load());
            audioPlayedFrames.store(other.audioPlayedFrames.load());
            audioEpochUs.store(other.audioEpochUs.load());
            audioSampleRate.store(other.audioSampleRate.load());

            // Leave the source object in a valid state
            other.framePtr = nullptr;
        }
        return *this;
    }
};

// Provide a custom non-member swap function for FrameLoaderState.
inline void swap(FrameLoaderState& a, FrameLoaderState& b) noexcept {
    using std::swap;

    // Swap all members that are swappable
    swap(a.framePool, b.framePool);
    swap(a.ring, b.ring);
    swap(a.nextReadTime, b.nextReadTime);
    swap(a.audioQueue, b.audioQueue);
    swap(a.audioSampleOffset, b.audioSampleOffset);
    swap(a.framePtr, b.framePtr);

    // --- Manually swap atomic members ---
    int writeIdxA = a.writeIdx.load();
    int writeIdxB = b.writeIdx.load();
    a.writeIdx.store(writeIdxB);
    b.writeIdx.store(writeIdxA);

    int readIdxA = a.readIdx.load();
    int readIdxB = b.readIdx.load();
    a.readIdx.store(readIdxB);
    b.readIdx.store(readIdxA);

    bool audioAvailableA = a.audioAvailable.load();
    bool audioAvailableB = b.audioAvailable.load();
    a.audioAvailable.store(audioAvailableB);
    b.audioAvailable.store(audioAvailableA);
    bool audioStartedA = a.audioStarted.load();
    bool audioStartedB = b.audioStarted.load();
    a.audioStarted.store(audioStartedB);
    b.audioStarted.store(audioStartedA);
    int64_t audioPlayedA = a.audioPlayedFrames.load();
    int64_t audioPlayedB = b.audioPlayedFrames.load();
    a.audioPlayedFrames.store(audioPlayedB);
    b.audioPlayedFrames.store(audioPlayedA);
    int64_t audioEpochA = a.audioEpochUs.load();
    int64_t audioEpochB = b.audioEpochUs.load();
    a.audioEpochUs.store(audioEpochB);
    b.audioEpochUs.store(audioEpochA);
    int audioRateA = a.audioSampleRate.load();
    int audioRateB = b.audioSampleRate.load();
    a.audioSampleRate.store(audioRateB);
    b.audioSampleRate.store(audioRateA);

    // --- DO NOT SWAP THREAD, MUTEXES, OR CONDITION VARIABLES ---
    // a.writerThread, b.writerThread
    // a.writerMutex, b.writerMutex
    // a.writerCv, b.writerCv
    // a.timingMutex, b.timingMutex
    // Swapping these doesn't make logical sense and is not allowed.
    // This custom swap implementation will satisfy the compiler for SparseSet::Remove.
}
