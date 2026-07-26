#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

enum class PerformanceSubsystem : std::size_t {
    EnvironmentDepth = 0,
    CameraCapture,
    LightEstimation,
    DepthPreparation,
    GeometryCompaction,
    IndexUpload,
    VideoDecode,
    TextureUpload,
    Rendering,
    OtherUpdate,
    Count,
};

enum class PerformanceDomain : std::size_t {
    Cpu = 0,
    Gpu,
    Count,
};

struct PerformanceTimingMetric {
    double averageMilliseconds = 0.0;
    std::uint64_t sampleCount = 0;

    bool HasSamples() const { return sampleCount != 0; }
};

struct PerformanceTimingSnapshot {
    using MetricArray = std::array<
        std::array<PerformanceTimingMetric, static_cast<std::size_t>(PerformanceDomain::Count)>,
        static_cast<std::size_t>(PerformanceSubsystem::Count)>;

    MetricArray metrics{};
    std::uint64_t generation = 0;
    bool valid = false;

    const PerformanceTimingMetric& Get(
            PerformanceSubsystem subsystem,
            PerformanceDomain domain) const {
        return metrics[static_cast<std::size_t>(subsystem)]
                      [static_cast<std::size_t>(domain)];
    }
};

class PerformanceTimingStats {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void SetEnabled(bool enabled) {
        const bool previous = enabled_.exchange(enabled, std::memory_order_acq_rel);
        if (previous != enabled) Reset();
    }

    bool IsEnabled() const {
        return enabled_.load(std::memory_order_acquire);
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        accumulated_ = {};
        published_ = {};
        published_.generation = generation_;
        windowStart_ = {};
        foreground_ = {};
        foregroundActive_ = false;
    }

    std::uint64_t Generation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }

    void Record(
            PerformanceSubsystem subsystem,
            PerformanceDomain domain,
            double milliseconds,
            bool foregroundUpdate = false) {
        if (!IsEnabled() || milliseconds < 0.0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_.load(std::memory_order_relaxed)) return;
        auto& value = accumulated_[Index(subsystem)][Index(domain)];
        value.totalMilliseconds += milliseconds;
        ++value.sampleCount;
        if (foregroundUpdate && foregroundActive_ &&
            domain == PerformanceDomain::Cpu) {
            foreground_[Index(subsystem)] += milliseconds;
        }
    }

    void BeginForegroundFrame() {
        if (!IsEnabled()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        foreground_ = {};
        foregroundActive_ = true;
    }

    void EndForegroundFrame(double totalMilliseconds) {
        if (!IsEnabled()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!foregroundActive_) return;
        double accounted = 0.0;
        accounted += foreground_[Index(PerformanceSubsystem::EnvironmentDepth)];
        accounted += foreground_[Index(PerformanceSubsystem::LightEstimation)];
        accounted += foreground_[Index(PerformanceSubsystem::TextureUpload)];
        const double other = totalMilliseconds > accounted
            ? totalMilliseconds - accounted
            : 0.0;
        auto& value = accumulated_[Index(PerformanceSubsystem::OtherUpdate)]
                                  [Index(PerformanceDomain::Cpu)];
        value.totalMilliseconds += other;
        ++value.sampleCount;
        foregroundActive_ = false;
    }

    bool PublishIfReady(TimePoint now) {
        if (!IsEnabled()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (windowStart_ == TimePoint{}) {
            windowStart_ = now;
            return false;
        }
        if (std::chrono::duration<double>(now - windowStart_).count() < 1.0) {
            return false;
        }
        published_ = {};
        published_.generation = generation_;
        published_.valid = true;
        for (std::size_t subsystem = 0; subsystem < accumulated_.size(); ++subsystem) {
            for (std::size_t domain = 0; domain < accumulated_[subsystem].size(); ++domain) {
                const auto& source = accumulated_[subsystem][domain];
                auto& destination = published_.metrics[subsystem][domain];
                destination.sampleCount = source.sampleCount;
                if (source.sampleCount != 0) {
                    destination.averageMilliseconds =
                        source.totalMilliseconds / static_cast<double>(source.sampleCount);
                }
            }
        }
        accumulated_ = {};
        windowStart_ = now;
        return true;
    }

    PerformanceTimingSnapshot Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return published_;
    }

    void InvalidateGpuWindow() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& subsystem : accumulated_) {
            subsystem[Index(PerformanceDomain::Gpu)] = {};
        }
        for (auto& subsystem : published_.metrics) {
            subsystem[Index(PerformanceDomain::Gpu)] = {};
        }
    }

private:
    struct Accumulator {
        double totalMilliseconds = 0.0;
        std::uint64_t sampleCount = 0;
    };
    using AccumulatorArray = std::array<
        std::array<Accumulator, static_cast<std::size_t>(PerformanceDomain::Count)>,
        static_cast<std::size_t>(PerformanceSubsystem::Count)>;

    static constexpr std::size_t Index(PerformanceSubsystem value) {
        return static_cast<std::size_t>(value);
    }

    static constexpr std::size_t Index(PerformanceDomain value) {
        return static_cast<std::size_t>(value);
    }

    mutable std::mutex mutex_;
    std::atomic<bool> enabled_{false};
    std::uint64_t generation_ = 1;
    AccumulatorArray accumulated_{};
    PerformanceTimingSnapshot published_{};
    TimePoint windowStart_{};
    std::array<double, static_cast<std::size_t>(PerformanceSubsystem::Count)> foreground_{};
    bool foregroundActive_ = false;
};

class ScopedCpuTimer {
public:
    ScopedCpuTimer(
            PerformanceTimingStats* stats,
            PerformanceSubsystem subsystem,
            bool foregroundUpdate = false)
        : stats_(stats),
          subsystem_(subsystem),
          foregroundUpdate_(foregroundUpdate),
          active_(stats != nullptr && stats->IsEnabled()) {
        if (active_) start_ = PerformanceTimingStats::Clock::now();
    }

    ~ScopedCpuTimer() {
        if (!active_) return;
        const double milliseconds = std::chrono::duration<double, std::milli>(
            PerformanceTimingStats::Clock::now() - start_).count();
        stats_->Record(
            subsystem_, PerformanceDomain::Cpu, milliseconds, foregroundUpdate_);
    }

    ScopedCpuTimer(const ScopedCpuTimer&) = delete;
    ScopedCpuTimer& operator=(const ScopedCpuTimer&) = delete;

private:
    PerformanceTimingStats* stats_ = nullptr;
    PerformanceSubsystem subsystem_ = PerformanceSubsystem::OtherUpdate;
    bool foregroundUpdate_ = false;
    bool active_ = false;
    PerformanceTimingStats::TimePoint start_{};
};
