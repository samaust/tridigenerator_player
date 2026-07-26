#include "Data/FrameTimingStats.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

FrameTimingStats::TimePoint At(double seconds) {
    return FrameTimingStats::TimePoint{} +
        std::chrono::duration_cast<FrameTimingStats::Clock::duration>(
            std::chrono::duration<double>(seconds));
}

bool Near(double actual, double expected, double tolerance = 0.001) {
    return std::abs(actual - expected) < tolerance;
}

void Check(bool condition, const std::string& message) {
    if (condition) {
        return;
    }
    std::cerr << "FrameTimingStatsTests failed: " << message << "\n";
    std::exit(EXIT_FAILURE);
}

void TestBasicPublicationAndReset() {
    FrameTimingStats stats;
    Check(!stats.HasPublishedSample(), "new stats must be unpublished");
    Check(!stats.AddFrame(At(10.0)), "first frame establishes the baseline");
    Check(!stats.AddFrame(At(10.5)), "half-second window must not publish");
    Check(stats.AddFrame(At(11.0)), "exact one-second window must publish");
    Check(stats.HasPublishedSample(), "published sample flag");
    Check(Near(stats.Fps(), 2.0), "two FPS");
    Check(Near(stats.AverageFrameMilliseconds(), 500.0), "500 ms average");
    Check(Near(stats.P50FrameMilliseconds(), 500.0), "500 ms p50");
    Check(Near(stats.P95FrameMilliseconds(), 500.0), "500 ms p95");
    Check(Near(stats.P99FrameMilliseconds(), 500.0), "500 ms p99");
    stats.SetPanelRefreshRate(72.0);
    Check(!stats.HasPublishedSample(), "rate change invalidates old interval window");

    stats.Reset();
    Check(!stats.HasPublishedSample(), "reset clears published sample");
    Check(!stats.HasPanelRefreshRate(), "reset clears panel rate");
    Check(stats.MissedFrameCount() == 0, "reset clears missed slots");
}

void TestStableCadence(double refreshRate) {
    FrameTimingStats stats;
    stats.SetPanelRefreshRate(refreshRate);
    Check(stats.HasPanelRefreshRate(), "valid panel rate is available");
    Check(!stats.AddFrame(At(20.0)), "stable cadence baseline");
    const int frameCount = static_cast<int>(refreshRate);
    for (int frame = 1; frame < frameCount; ++frame) {
        Check(
            !stats.AddFrame(At(20.0 + frame / refreshRate)),
            "stable cadence must wait for one-second window");
    }
    Check(
        stats.AddFrame(At(21.0)),
        "stable cadence exact one-second boundary");
    Check(Near(stats.Fps(), refreshRate, 0.01), "stable cadence FPS");
    Check(
        Near(stats.AverageFrameMilliseconds(), 1000.0 / refreshRate, 0.01),
        "stable cadence average interval");
    Check(
        Near(stats.P50FrameMilliseconds(), 1000.0 / refreshRate, 0.01),
        "stable cadence p50");
    Check(
        Near(stats.P95FrameMilliseconds(), 1000.0 / refreshRate, 0.01),
        "stable cadence p95");
    Check(
        Near(stats.P99FrameMilliseconds(), 1000.0 / refreshRate, 0.01),
        "stable cadence p99");
    Check(stats.MissedFrameCount() == 0, "stable cadence has no missed slots");
}

void TestNearestRankPercentiles() {
    FrameTimingStats stats;
    Check(!stats.AddFrame(At(0.0)), "percentile baseline");
    Check(!stats.AddFrame(At(0.1)), "100 ms interval");
    Check(!stats.AddFrame(At(0.3)), "200 ms interval");
    Check(!stats.AddFrame(At(0.6)), "300 ms interval");
    Check(stats.AddFrame(At(1.0)), "400 ms interval publishes");
    Check(Near(stats.P50FrameMilliseconds(), 200.0), "nearest-rank p50");
    Check(Near(stats.P95FrameMilliseconds(), 400.0), "nearest-rank p95");
    Check(Near(stats.P99FrameMilliseconds(), 400.0), "nearest-rank p99");
    Check(stats.MissedFrameCount() == 0, "unknown panel rate does not count misses");
}

void TestMissedSlotsAndRateChanges() {
    FrameTimingStats stats;
    stats.SetPanelRefreshRate(10.0);
    Check(!stats.AddFrame(At(0.0)), "miss baseline");
    Check(!stats.AddFrame(At(0.1)), "one panel period");
    Check(!stats.AddFrame(At(0.3)), "two panel periods");
    Check(!stats.AddFrame(At(0.6)), "three panel periods");
    Check(stats.MissedFrameCount() == 3, "one plus two skipped panel slots");

    stats.SetPanelRefreshRate(20.0);
    Check(stats.MissedFrameCount() == 3, "rate change preserves session miss total");
    Check(!stats.AddFrame(At(0.61)), "rate change establishes a new baseline");
    Check(!stats.AddFrame(At(0.66)), "one period at new rate");
    Check(stats.MissedFrameCount() == 3, "rate transition creates no false miss");

    stats.SetPanelRefreshRate(0.0);
    Check(!stats.HasPanelRefreshRate(), "invalid rate disables miss accounting");
}

void TestNonMonotonicTimestamp() {
    FrameTimingStats stats;
    Check(!stats.AddFrame(At(10.0)), "non-monotonic baseline");
    Check(!stats.AddFrame(At(9.0)), "backward timestamp is ignored");
    Check(!stats.AddFrame(At(9.25)), "backward timestamp becomes new baseline");
    Check(!stats.HasPublishedSample(), "invalid interval does not publish");
}

} // namespace

int main() {
    TestBasicPublicationAndReset();
    TestStableCadence(72.0);
    TestStableCadence(90.0);
    TestNearestRankPercentiles();
    TestMissedSlotsAndRateChanges();
    TestNonMonotonicTimestamp();
    return EXIT_SUCCESS;
}
