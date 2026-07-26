#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

enum class PerformanceSubsystem : std::size_t {
    EnvironmentDepth = 0,
    CameraCapture,
    LightEstimation,
    DepthPreparation,
    GeometryCompaction,
    IndexUpload,
    ColorDecode,
    ColorCopy,
    ColorHardwareOutputWait,
    AlphaDecode,
    AlphaCopy,
    DepthDecode,
    DepthConvertCopy,
    DemuxAudio,
    VideoDecode,
    TextureStaging,
    HardwareColorConversion,
    TextureUpload,
    Rendering,
    XrWaitFrame,
    SwapchainAcquire,
    XrEndFrame,
    DiagnosticRefresh,
    OtherUpdate,
    Count,
};

enum class PerformanceDomain : std::size_t {
    Cpu = 0,
    Gpu,
    Count,
};

enum class UpdatePhase : std::size_t {
    DiagnosticsBookkeeping = 0,
    CoreScene,
    InputControl,
    FrameLoaderAudio,
    InteractionHaptics,
    ScenePreparation,
    EnvironmentDepth,
    LightEstimation,
    UnlitGeometry,
    UiPointer,
    Count,
};

constexpr std::size_t UpdatePhaseCount =
    static_cast<std::size_t>(UpdatePhase::Count);

struct PerformanceTimingMetric {
    double averageMilliseconds = 0.0;
    double p95Milliseconds = 0.0;
    double maximumMilliseconds = 0.0;
    std::uint64_t sampleCount = 0;

    bool HasSamples() const { return sampleCount != 0; }
};

struct UpdateTimingSnapshot {
    PerformanceTimingMetric total{};
    std::array<PerformanceTimingMetric, UpdatePhaseCount> phases{};
    PerformanceTimingMetric residual{};
    double budgetMilliseconds = 1000.0 / 72.0;
    std::uint64_t overBudgetCount = 0;
    std::array<double, UpdatePhaseCount> slowestFramePhases{};
    double slowestFrameResidualMilliseconds = 0.0;
    std::size_t slowestFrameDominantContributor = UpdatePhaseCount;
};

struct PerformanceTimingSnapshot {
    using MetricArray = std::array<
        std::array<PerformanceTimingMetric, static_cast<std::size_t>(PerformanceDomain::Count)>,
        static_cast<std::size_t>(PerformanceSubsystem::Count)>;

    MetricArray metrics{};
    UpdateTimingSnapshot update{};
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
        for (auto& subsystem : accumulated_) {
            for (auto& domain : subsystem) domain.samples.clear();
        }
        published_ = {};
        published_.generation = generation_;
        updateAccumulated_ = {};
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
        value.samples.push_back(milliseconds);
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
        value.samples.push_back(other);
        foregroundActive_ = false;
    }

    void RecordUpdateFrame(
            const std::array<double, UpdatePhaseCount>& phaseMilliseconds,
            double totalMilliseconds,
            double budgetMilliseconds) {
        if (!IsEnabled() || totalMilliseconds < 0.0 ||
            budgetMilliseconds <= 0.0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_.load(std::memory_order_relaxed)) return;

        double accountedMilliseconds = 0.0;
        for (std::size_t index = 0; index < UpdatePhaseCount; ++index) {
            const double value = std::max(0.0, phaseMilliseconds[index]);
            AddSample(updateAccumulated_.phases[index], value);
            accountedMilliseconds += value;
        }
        const double residualMilliseconds =
            std::max(0.0, totalMilliseconds - accountedMilliseconds);
        AddSample(updateAccumulated_.total, totalMilliseconds);
        AddSample(updateAccumulated_.residual, residualMilliseconds);
        AddSample(
            accumulated_[Index(PerformanceSubsystem::OtherUpdate)]
                        [Index(PerformanceDomain::Cpu)],
            residualMilliseconds);
        updateAccumulated_.budgetMilliseconds = budgetMilliseconds;
        if (totalMilliseconds > budgetMilliseconds) {
            ++updateAccumulated_.overBudgetCount;
        }
        if (totalMilliseconds > updateAccumulated_.slowestFrameMilliseconds) {
            updateAccumulated_.slowestFrameMilliseconds = totalMilliseconds;
            updateAccumulated_.slowestFramePhases = phaseMilliseconds;
            updateAccumulated_.slowestFrameResidualMilliseconds =
                residualMilliseconds;
            std::size_t dominant = UpdatePhaseCount;
            double dominantMilliseconds = residualMilliseconds;
            for (std::size_t index = 0; index < UpdatePhaseCount; ++index) {
                if (phaseMilliseconds[index] > dominantMilliseconds) {
                    dominantMilliseconds = phaseMilliseconds[index];
                    dominant = index;
                }
            }
            updateAccumulated_.slowestFrameDominantContributor = dominant;
        }
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
                PublishMetric(source, destination);
            }
        }
        PublishMetric(updateAccumulated_.total, published_.update.total);
        PublishMetric(updateAccumulated_.residual, published_.update.residual);
        for (std::size_t index = 0; index < UpdatePhaseCount; ++index) {
            PublishMetric(
                updateAccumulated_.phases[index],
                published_.update.phases[index]);
        }
        published_.update.budgetMilliseconds =
            updateAccumulated_.budgetMilliseconds;
        published_.update.overBudgetCount =
            updateAccumulated_.overBudgetCount;
        published_.update.slowestFramePhases =
            updateAccumulated_.slowestFramePhases;
        published_.update.slowestFrameResidualMilliseconds =
            updateAccumulated_.slowestFrameResidualMilliseconds;
        published_.update.slowestFrameDominantContributor =
            updateAccumulated_.slowestFrameDominantContributor;
        accumulated_ = {};
        for (auto& subsystem : accumulated_) {
            for (auto& domain : subsystem) domain.samples.clear();
        }
        updateAccumulated_ = {};
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
        std::vector<double> samples;
    };
    struct UpdateAccumulator {
        Accumulator total{};
        std::array<Accumulator, UpdatePhaseCount> phases{};
        Accumulator residual{};
        double budgetMilliseconds = 1000.0 / 72.0;
        std::uint64_t overBudgetCount = 0;
        double slowestFrameMilliseconds = -1.0;
        std::array<double, UpdatePhaseCount> slowestFramePhases{};
        double slowestFrameResidualMilliseconds = 0.0;
        std::size_t slowestFrameDominantContributor = UpdatePhaseCount;
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

    static void AddSample(Accumulator& accumulator, double milliseconds) {
        accumulator.totalMilliseconds += milliseconds;
        ++accumulator.sampleCount;
        accumulator.samples.push_back(milliseconds);
    }

    static void PublishMetric(
            const Accumulator& source,
            PerformanceTimingMetric& destination) {
        destination.sampleCount = source.sampleCount;
        if (source.sampleCount == 0) return;
        destination.averageMilliseconds =
            source.totalMilliseconds / static_cast<double>(source.sampleCount);
        if (source.samples.empty()) {
            destination.p95Milliseconds = destination.averageMilliseconds;
            destination.maximumMilliseconds = destination.averageMilliseconds;
            return;
        }
        std::vector<double> sorted = source.samples;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t rank = static_cast<std::size_t>(
            std::ceil(0.95 * static_cast<double>(sorted.size())));
        destination.p95Milliseconds = sorted[std::min(
            sorted.size() - 1,
            rank > 0 ? rank - 1 : 0)];
        destination.maximumMilliseconds = sorted.back();
    }

    mutable std::mutex mutex_;
    std::atomic<bool> enabled_{false};
    std::uint64_t generation_ = 1;
    AccumulatorArray accumulated_{};
    UpdateAccumulator updateAccumulated_{};
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
