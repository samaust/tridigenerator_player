#include "Data/FrameTimingStats.h"

#include <cassert>
#include <chrono>
#include <cmath>

namespace {

FrameTimingStats::TimePoint At(double seconds) {
    return FrameTimingStats::TimePoint{} +
        std::chrono::duration_cast<FrameTimingStats::Clock::duration>(
            std::chrono::duration<double>(seconds));
}

bool Near(double actual, double expected) {
    return std::abs(actual - expected) < 0.001;
}

} // namespace

int main() {
    FrameTimingStats stats;
    assert(!stats.HasPublishedSample());
    assert(!stats.AddFrame(At(10.0)));
    assert(!stats.AddFrame(At(10.5)));
    assert(stats.AddFrame(At(11.0)));
    assert(stats.HasPublishedSample());
    assert(Near(stats.Fps(), 2.0));
    assert(Near(stats.AverageFrameMilliseconds(), 500.0));

    // A non-monotonic timestamp is ignored and becomes the new baseline.
    assert(!stats.AddFrame(At(10.0)));
    assert(!stats.AddFrame(At(10.25)));

    stats.Reset();
    assert(!stats.HasPublishedSample());
    assert(!stats.AddFrame(At(20.0)));
    for (int frame = 1; frame < 60; ++frame) {
        assert(!stats.AddFrame(At(20.0 + frame / 60.0)));
    }
    assert(stats.AddFrame(At(21.0)));
    assert(Near(stats.Fps(), 60.0));
    assert(Near(stats.AverageFrameMilliseconds(), 1000.0 / 60.0));
    return 0;
}
