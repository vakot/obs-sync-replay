#pragma once

#include "timeline/master-frame-timeline/master-frame-timeline.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace obs_sync_replay {

// Observes the single libobs graphics cadence and dispatches its immutable
// temporal identity. Frame sinks execute on libobs's graphics thread.
class MasterFrameCoordinator final {
public:
    using FrameSink = std::function<void(const MasterFrame &frame)>;

    explicit MasterFrameCoordinator(FrameSink frame_sink);
    ~MasterFrameCoordinator();

    MasterFrameCoordinator(const MasterFrameCoordinator &) = delete;
    MasterFrameCoordinator &operator=(const MasterFrameCoordinator &) = delete;

    void Start();
    void Stop();

private:
    static void OnObsTick(void *parameter, float seconds);
    void ObserveObsTick();
    void LogTimingConfiguration();

    FrameSink frame_sink_;
    detail::MasterFrameTimeline timeline_;
    std::optional<MasterFramePts> last_observed_pts_ns_;
    uint64_t frame_interval_ns_ = 0;
    uint32_t lagged_frames_ = 0;
    bool timing_configuration_logged_ = false;
    bool running_ = false;
};

} // namespace obs_sync_replay
