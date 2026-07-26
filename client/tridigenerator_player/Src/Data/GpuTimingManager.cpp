#include "GpuTimingManager.h"

#include <cstring>
#include <utility>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#endif

GpuTimingManager::GpuTimingManager(std::shared_ptr<PerformanceTimingStats> stats)
    : stats_(std::move(stats)) {}

GpuTimingManager::~GpuTimingManager() = default;

bool GpuTimingManager::Init() {
    if (initialized_) return supported_;
    initialized_ = true;
#if defined(__ANDROID__)
    const char* extensions =
        reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (extensions == nullptr ||
        std::strstr(extensions, "GL_EXT_disjoint_timer_query") == nullptr) {
        return false;
    }
    genQueries_ = reinterpret_cast<GenQueriesFn>(
        eglGetProcAddress("glGenQueriesEXT"));
    deleteQueries_ = reinterpret_cast<DeleteQueriesFn>(
        eglGetProcAddress("glDeleteQueriesEXT"));
    beginQuery_ = reinterpret_cast<BeginQueryFn>(
        eglGetProcAddress("glBeginQueryEXT"));
    endQuery_ = reinterpret_cast<EndQueryFn>(
        eglGetProcAddress("glEndQueryEXT"));
    getQueryObject_ = reinterpret_cast<GetQueryObjectFn>(
        eglGetProcAddress("glGetQueryObjectuivEXT"));
    getQueryObject64_ = reinterpret_cast<GetQueryObject64Fn>(
        eglGetProcAddress("glGetQueryObjectui64vEXT"));
    if (!genQueries_ || !deleteQueries_ || !beginQuery_ || !endQuery_ ||
        !getQueryObject_ || !getQueryObject64_) {
        return false;
    }
    std::array<unsigned int, QueryCount> ids{};
    genQueries_(static_cast<int>(ids.size()), ids.data());
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        slots_[index].id = ids[index];
        if (ids[index] == 0) {
            Shutdown();
            initialized_ = true;
            return false;
        }
    }
    supported_ = true;
#endif
    return supported_;
}

void GpuTimingManager::Shutdown() {
    if (!initialized_) return;
#if defined(__ANDROID__)
    if (deleteQueries_) {
        std::array<unsigned int, QueryCount> ids{};
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            ids[index] = slots_[index].id;
        }
        deleteQueries_(static_cast<int>(ids.size()), ids.data());
    }
#endif
    slots_ = {};
    activeSlot_ = -1;
    supported_ = false;
    initialized_ = false;
}

void GpuTimingManager::ResetQueries() {
#if defined(__ANDROID__)
    if (!supported_ || !deleteQueries_ || !genQueries_) return;
    std::array<unsigned int, QueryCount> ids{};
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        ids[index] = slots_[index].id;
    }
    deleteQueries_(static_cast<int>(ids.size()), ids.data());
    ids = {};
    genQueries_(static_cast<int>(ids.size()), ids.data());
    slots_ = {};
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        slots_[index].id = ids[index];
        if (ids[index] == 0) {
            supported_ = false;
        }
    }
    activeSlot_ = -1;
#endif
}

void GpuTimingManager::Poll() {
#if defined(__ANDROID__)
    if (!supported_ || activeSlot_ >= 0) return;
    int disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    if (disjoint != 0) {
        if (stats_) stats_->InvalidateGpuWindow();
        ResetQueries();
        return;
    }
    for (auto& slot : slots_) {
        if (!slot.pending) continue;
        unsigned int available = 0;
        getQueryObject_(slot.id, GL_QUERY_RESULT_AVAILABLE_EXT, &available);
        if (available == 0) continue;
        std::uint64_t nanoseconds = 0;
        getQueryObject64_(slot.id, GL_QUERY_RESULT_EXT, &nanoseconds);
        if (stats_ && stats_->IsEnabled() &&
            slot.generation == stats_->Generation()) {
            stats_->Record(
                slot.subsystem,
                PerformanceDomain::Gpu,
                static_cast<double>(nanoseconds) / 1000000.0);
        }
        slot.pending = false;
    }
#endif
}

bool GpuTimingManager::Begin(PerformanceSubsystem subsystem) {
#if defined(__ANDROID__)
    if (!supported_ || activeSlot_ >= 0 || !stats_ || !stats_->IsEnabled()) {
        return false;
    }
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        auto& slot = slots_[index];
        if (slot.pending || slot.id == 0) continue;
        slot.subsystem = subsystem;
        slot.generation = stats_->Generation();
        beginQuery_(GL_TIME_ELAPSED_EXT, slot.id);
        activeSlot_ = static_cast<int>(index);
        return true;
    }
#else
    (void)subsystem;
#endif
    return false;
}

void GpuTimingManager::End() {
#if defined(__ANDROID__)
    if (!supported_ || activeSlot_ < 0) return;
    endQuery_(GL_TIME_ELAPSED_EXT);
    slots_[static_cast<std::size_t>(activeSlot_)].pending = true;
    activeSlot_ = -1;
#endif
}
