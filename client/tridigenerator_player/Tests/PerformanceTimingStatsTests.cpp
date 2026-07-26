#include "Data/PerformanceTimingStats.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace {

PerformanceTimingStats::TimePoint At(double seconds) {
    return PerformanceTimingStats::TimePoint{} +
        std::chrono::duration_cast<PerformanceTimingStats::Clock::duration>(
            std::chrono::duration<double>(seconds));
}

bool Near(double actual, double expected) {
    return std::abs(actual - expected) < 0.001;
}

} // namespace

int main() {
    PerformanceTimingStats stats;
    assert(!stats.IsEnabled());
    stats.Record(
        PerformanceSubsystem::VideoDecode,
        PerformanceDomain::Cpu,
        9.0);
    assert(!stats.Snapshot().valid);

    stats.SetEnabled(true);
    assert(stats.IsEnabled());
    const std::uint64_t generation = stats.Generation();
    assert(!stats.PublishIfReady(At(10.0)));

    stats.Record(
        PerformanceSubsystem::VideoDecode,
        PerformanceDomain::Cpu,
        1.0);
    stats.Record(
        PerformanceSubsystem::VideoDecode,
        PerformanceDomain::Cpu,
        3.0);
    stats.Record(
        PerformanceSubsystem::Rendering,
        PerformanceDomain::Gpu,
        4.0);
    stats.Record(
        PerformanceSubsystem::XrWaitFrame,
        PerformanceDomain::Cpu,
        12.0);
    stats.Record(
        PerformanceSubsystem::XrWaitFrame,
        PerformanceDomain::Cpu,
        14.0);
    stats.Record(
        PerformanceSubsystem::SwapchainAcquire,
        PerformanceDomain::Cpu,
        0.25);
    stats.Record(
        PerformanceSubsystem::XrEndFrame,
        PerformanceDomain::Cpu,
        0.5);
    stats.Record(
        PerformanceSubsystem::DiagnosticRefresh,
        PerformanceDomain::Cpu,
        3.0);

    stats.BeginForegroundFrame();
    stats.Record(
        PerformanceSubsystem::EnvironmentDepth,
        PerformanceDomain::Cpu,
        2.0,
        true);
    stats.Record(
        PerformanceSubsystem::TextureUpload,
        PerformanceDomain::Cpu,
        1.0,
        true);
    stats.EndForegroundFrame(10.0);

    assert(stats.PublishIfReady(At(11.0)));
    const auto snapshot = stats.Snapshot();
    assert(snapshot.valid);
    assert(snapshot.generation == generation);
    assert(snapshot.Get(
        PerformanceSubsystem::VideoDecode,
        PerformanceDomain::Cpu).sampleCount == 2);
    assert(Near(snapshot.Get(
        PerformanceSubsystem::VideoDecode,
        PerformanceDomain::Cpu).averageMilliseconds, 2.0));
    assert(Near(snapshot.Get(
        PerformanceSubsystem::VideoDecode,
        PerformanceDomain::Cpu).p95Milliseconds, 3.0));
    assert(Near(snapshot.Get(
        PerformanceSubsystem::VideoDecode,
        PerformanceDomain::Cpu).maximumMilliseconds, 3.0));
    assert(Near(snapshot.Get(
        PerformanceSubsystem::Rendering,
        PerformanceDomain::Gpu).averageMilliseconds, 4.0));
    assert(Near(snapshot.Get(
        PerformanceSubsystem::XrWaitFrame,
        PerformanceDomain::Cpu).averageMilliseconds, 13.0));
    assert(Near(snapshot.Get(
        PerformanceSubsystem::XrWaitFrame,
        PerformanceDomain::Cpu).p95Milliseconds, 14.0));
    assert(Near(snapshot.Get(
        PerformanceSubsystem::SwapchainAcquire,
        PerformanceDomain::Cpu).averageMilliseconds, 0.25));
    assert(Near(snapshot.Get(
        PerformanceSubsystem::XrEndFrame,
        PerformanceDomain::Cpu).averageMilliseconds, 0.5));
    assert(Near(snapshot.Get(
        PerformanceSubsystem::DiagnosticRefresh,
        PerformanceDomain::Cpu).averageMilliseconds, 3.0));
    assert(Near(snapshot.Get(
        PerformanceSubsystem::OtherUpdate,
        PerformanceDomain::Cpu).averageMilliseconds, 7.0));
    assert(Near(snapshot.Get(
        PerformanceSubsystem::OtherUpdate,
        PerformanceDomain::Cpu).p95Milliseconds, 7.0));
    assert(!snapshot.Get(
        PerformanceSubsystem::CameraCapture,
        PerformanceDomain::Cpu).HasSamples());

    stats.InvalidateGpuWindow();
    assert(!stats.Snapshot().Get(
        PerformanceSubsystem::Rendering,
        PerformanceDomain::Gpu).HasSamples());

    stats.Reset();
    assert(stats.Generation() != generation);
    assert(!stats.Snapshot().valid);

    assert(!stats.PublishIfReady(At(20.0)));
    constexpr int ThreadCount = 4;
    constexpr int SamplesPerThread = 100;
    std::vector<std::thread> workers;
    for (int thread = 0; thread < ThreadCount; ++thread) {
        workers.emplace_back([&]() {
            for (int sample = 0; sample < SamplesPerThread; ++sample) {
                stats.Record(
                    PerformanceSubsystem::CameraCapture,
                    PerformanceDomain::Cpu,
                    0.5);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    assert(stats.PublishIfReady(At(21.0)));
    const auto concurrent = stats.Snapshot().Get(
        PerformanceSubsystem::CameraCapture,
        PerformanceDomain::Cpu);
    assert(concurrent.sampleCount == ThreadCount * SamplesPerThread);
    assert(Near(concurrent.averageMilliseconds, 0.5));

    stats.Reset();
    assert(!stats.PublishIfReady(At(30.0)));
    std::array<double, UpdatePhaseCount> firstPhases{};
    firstPhases[static_cast<std::size_t>(UpdatePhase::CoreScene)] = 2.0;
    firstPhases[static_cast<std::size_t>(UpdatePhase::UiPointer)] = 3.0;
    stats.RecordUpdateFrame(firstPhases, 10.0, 8.0);
    // A nested subsystem sample must not change the update residual.
    stats.Record(
        PerformanceSubsystem::TextureUpload,
        PerformanceDomain::Cpu,
        20.0,
        true);
    std::array<double, UpdatePhaseCount> secondPhases{};
    secondPhases[static_cast<std::size_t>(UpdatePhase::CoreScene)] = 8.0;
    stats.RecordUpdateFrame(secondPhases, 12.0, 8.0);
    assert(stats.PublishIfReady(At(31.0)));
    const auto updateSnapshot = stats.Snapshot();
    assert(updateSnapshot.update.total.sampleCount == 2);
    assert(Near(updateSnapshot.update.total.averageMilliseconds, 11.0));
    assert(Near(updateSnapshot.update.total.p95Milliseconds, 12.0));
    assert(Near(updateSnapshot.update.total.maximumMilliseconds, 12.0));
    assert(updateSnapshot.update.overBudgetCount == 2);
    assert(Near(updateSnapshot.update.budgetMilliseconds, 8.0));
    assert(Near(updateSnapshot.update.residual.averageMilliseconds, 4.5));
    assert(Near(updateSnapshot.Get(
        PerformanceSubsystem::OtherUpdate,
        PerformanceDomain::Cpu).averageMilliseconds, 4.5));
    assert(updateSnapshot.update.slowestFrameDominantContributor ==
        static_cast<std::size_t>(UpdatePhase::CoreScene));
    assert(Near(
        updateSnapshot.update.slowestFramePhases[
            static_cast<std::size_t>(UpdatePhase::CoreScene)],
        8.0));

    stats.Reset();
    assert(!stats.PublishIfReady(At(40.0)));
    std::array<double, UpdatePhaseCount> residualDominantPhases{};
    residualDominantPhases[
        static_cast<std::size_t>(UpdatePhase::DiagnosticsBookkeeping)] = 1.0;
    stats.RecordUpdateFrame(residualDominantPhases, 9.0, 13.89);
    assert(stats.PublishIfReady(At(41.0)));
    const auto residualDominant = stats.Snapshot().update;
    assert(residualDominant.overBudgetCount == 0);
    assert(residualDominant.slowestFrameDominantContributor ==
        UpdatePhaseCount);
    assert(Near(residualDominant.slowestFrameResidualMilliseconds, 8.0));

    stats.SetEnabled(false);
    stats.Record(
        PerformanceSubsystem::VideoDecode,
        PerformanceDomain::Cpu,
        100.0);
    stats.RecordUpdateFrame(residualDominantPhases, 100.0, 13.89);
    assert(!stats.Snapshot().valid);
    return 0;
}
