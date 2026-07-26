#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "PerformanceTimingStats.h"

class GpuTimingManager {
public:
    explicit GpuTimingManager(std::shared_ptr<PerformanceTimingStats> stats);
    ~GpuTimingManager();

    bool Init();
    void Shutdown();
    void Poll();
    bool Begin(PerformanceSubsystem subsystem);
    void End();
    bool IsSupported() const { return supported_; }

private:
    static constexpr std::size_t QueryCount = 12;

    struct QuerySlot {
        unsigned int id = 0;
        PerformanceSubsystem subsystem = PerformanceSubsystem::Rendering;
        std::uint64_t generation = 0;
        bool pending = false;
    };

    void ResetQueries();

    std::shared_ptr<PerformanceTimingStats> stats_;
    std::array<QuerySlot, QueryCount> slots_{};
    int activeSlot_ = -1;
    bool initialized_ = false;
    bool supported_ = false;

#if defined(__ANDROID__)
    using GenQueriesFn = void (*)(int, unsigned int*);
    using DeleteQueriesFn = void (*)(int, const unsigned int*);
    using BeginQueryFn = void (*)(unsigned int, unsigned int);
    using EndQueryFn = void (*)(unsigned int);
    using GetQueryObjectFn = void (*)(unsigned int, unsigned int, unsigned int*);
    using GetQueryObject64Fn = void (*)(unsigned int, unsigned int, std::uint64_t*);

    GenQueriesFn genQueries_ = nullptr;
    DeleteQueriesFn deleteQueries_ = nullptr;
    BeginQueryFn beginQuery_ = nullptr;
    EndQueryFn endQuery_ = nullptr;
    GetQueryObjectFn getQueryObject_ = nullptr;
    GetQueryObject64Fn getQueryObject64_ = nullptr;
#endif
};

class ScopedGpuTimer {
public:
    ScopedGpuTimer(GpuTimingManager* manager, PerformanceSubsystem subsystem)
        : manager_(manager),
          active_(manager != nullptr && manager->Begin(subsystem)) {}

    ~ScopedGpuTimer() {
        if (active_) manager_->End();
    }

    ScopedGpuTimer(const ScopedGpuTimer&) = delete;
    ScopedGpuTimer& operator=(const ScopedGpuTimer&) = delete;

private:
    GpuTimingManager* manager_ = nullptr;
    bool active_ = false;
};
