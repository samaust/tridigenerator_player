#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

class FrameTimingStats {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void Reset() {
        hasPreviousFrame_ = false;
        windowStart_ = {};
        previousFrame_ = {};
        sampleCount_ = 0;
        accumulatedSeconds_ = 0.0;
        frameIntervalsMilliseconds_.clear();
        fps_ = 0.0;
        averageFrameMilliseconds_ = 0.0;
        p50FrameMilliseconds_ = 0.0;
        p95FrameMilliseconds_ = 0.0;
        p99FrameMilliseconds_ = 0.0;
        panelRefreshRateHz_ = 0.0;
        missedFrameCount_ = 0;
        hasPublishedSample_ = false;
    }

    void SetPanelRefreshRate(double refreshRateHz) {
        const double sanitized =
            std::isfinite(refreshRateHz) && refreshRateHz > 0.0
            ? refreshRateHz
            : 0.0;
        if (sanitized == panelRefreshRateHz_) {
            return;
        }
        panelRefreshRateHz_ = sanitized;
        ResetSamplingWindow();
        fps_ = 0.0;
        averageFrameMilliseconds_ = 0.0;
        p50FrameMilliseconds_ = 0.0;
        p95FrameMilliseconds_ = 0.0;
        p99FrameMilliseconds_ = 0.0;
        hasPublishedSample_ = false;
    }

    bool AddFrame(TimePoint timestamp) {
        if (!hasPreviousFrame_) {
            previousFrame_ = timestamp;
            windowStart_ = timestamp;
            hasPreviousFrame_ = true;
            return false;
        }

        const double frameSeconds =
            std::chrono::duration<double>(timestamp - previousFrame_).count();
        if (frameSeconds <= 0.0) {
            ResetSamplingWindow();
            previousFrame_ = timestamp;
            windowStart_ = timestamp;
            hasPreviousFrame_ = true;
            return false;
        }
        previousFrame_ = timestamp;

        accumulatedSeconds_ += frameSeconds;
        ++sampleCount_;
        frameIntervalsMilliseconds_.push_back(frameSeconds * 1000.0);
        if (panelRefreshRateHz_ > 0.0) {
            const long long elapsedPanelSlots =
                std::llround(frameSeconds * panelRefreshRateHz_);
            if (elapsedPanelSlots > 1) {
                missedFrameCount_ +=
                    static_cast<std::uint64_t>(elapsedPanelSlots - 1);
            }
        }

        const double windowSeconds =
            std::chrono::duration<double>(timestamp - windowStart_).count();
        if (windowSeconds < 1.0) {
            return false;
        }

        fps_ = static_cast<double>(sampleCount_) / windowSeconds;
        averageFrameMilliseconds_ =
            accumulatedSeconds_ * 1000.0 / static_cast<double>(sampleCount_);
        std::sort(
            frameIntervalsMilliseconds_.begin(),
            frameIntervalsMilliseconds_.end());
        p50FrameMilliseconds_ = Percentile(0.50);
        p95FrameMilliseconds_ = Percentile(0.95);
        p99FrameMilliseconds_ = Percentile(0.99);
        hasPublishedSample_ = true;
        ResetSamplingWindow();
        previousFrame_ = timestamp;
        windowStart_ = timestamp;
        hasPreviousFrame_ = true;
        return true;
    }

    bool HasPublishedSample() const { return hasPublishedSample_; }
    bool HasPanelRefreshRate() const { return panelRefreshRateHz_ > 0.0; }
    double Fps() const { return fps_; }
    double AverageFrameMilliseconds() const { return averageFrameMilliseconds_; }
    double P50FrameMilliseconds() const { return p50FrameMilliseconds_; }
    double P95FrameMilliseconds() const { return p95FrameMilliseconds_; }
    double P99FrameMilliseconds() const { return p99FrameMilliseconds_; }
    std::uint64_t MissedFrameCount() const { return missedFrameCount_; }

private:
    double Percentile(double percentile) const {
        if (frameIntervalsMilliseconds_.empty()) {
            return 0.0;
        }
        const std::size_t rank = static_cast<std::size_t>(
            std::ceil(percentile * frameIntervalsMilliseconds_.size()));
        return frameIntervalsMilliseconds_[
            std::min(
                frameIntervalsMilliseconds_.size() - 1,
                rank > 0 ? rank - 1 : 0)];
    }

    void ResetSamplingWindow() {
        hasPreviousFrame_ = false;
        sampleCount_ = 0;
        accumulatedSeconds_ = 0.0;
        frameIntervalsMilliseconds_.clear();
    }

    TimePoint windowStart_{};
    TimePoint previousFrame_{};
    bool hasPreviousFrame_ = false;
    std::size_t sampleCount_ = 0;
    double accumulatedSeconds_ = 0.0;
    std::vector<double> frameIntervalsMilliseconds_;
    double fps_ = 0.0;
    double averageFrameMilliseconds_ = 0.0;
    double p50FrameMilliseconds_ = 0.0;
    double p95FrameMilliseconds_ = 0.0;
    double p99FrameMilliseconds_ = 0.0;
    double panelRefreshRateHz_ = 0.0;
    std::uint64_t missedFrameCount_ = 0;
    bool hasPublishedSample_ = false;
};
