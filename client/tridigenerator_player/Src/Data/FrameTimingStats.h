#pragma once

#include <chrono>
#include <cstddef>

class FrameTimingStats {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void Reset() {
        hasPreviousFrame_ = false;
        sampleCount_ = 0;
        accumulatedSeconds_ = 0.0;
        fps_ = 0.0;
        averageFrameMilliseconds_ = 0.0;
        hasPublishedSample_ = false;
    }

    bool AddFrame(TimePoint timestamp) {
        if (!hasPreviousFrame_) {
            previousFrame_ = timestamp;
            hasPreviousFrame_ = true;
            return false;
        }

        const double frameSeconds =
            std::chrono::duration<double>(timestamp - previousFrame_).count();
        previousFrame_ = timestamp;
        if (frameSeconds <= 0.0) {
            return false;
        }

        accumulatedSeconds_ += frameSeconds;
        ++sampleCount_;
        if (accumulatedSeconds_ < 1.0) {
            return false;
        }

        fps_ = static_cast<double>(sampleCount_) / accumulatedSeconds_;
        averageFrameMilliseconds_ =
            accumulatedSeconds_ * 1000.0 / static_cast<double>(sampleCount_);
        sampleCount_ = 0;
        accumulatedSeconds_ = 0.0;
        hasPublishedSample_ = true;
        return true;
    }

    bool HasPublishedSample() const { return hasPublishedSample_; }
    double Fps() const { return fps_; }
    double AverageFrameMilliseconds() const { return averageFrameMilliseconds_; }

private:
    TimePoint previousFrame_{};
    bool hasPreviousFrame_ = false;
    std::size_t sampleCount_ = 0;
    double accumulatedSeconds_ = 0.0;
    double fps_ = 0.0;
    double averageFrameMilliseconds_ = 0.0;
    bool hasPublishedSample_ = false;
};
