#pragma once

#include "timeline/master-frame.hpp"

#include <optional>

namespace obs_sync_replay {

enum class MasterFrameObservationResult {
    Accepted,
    NonMonotonicPts,
    FrameIdExhausted,
};

enum class MasterFrameTimingConfigurationResult {
    Initialized,
    Unchanged,
    Changed,
    InvalidInterval,
};

namespace detail {

// Internal identity issuer. It has no OBS dependency so its transition rules
// are covered by deterministic tests.
class MasterFrameTimeline final {
public:
    MasterFrameObservationResult Observe(MasterFramePts pts_ns, std::optional<MasterFrame> &frame) noexcept;
    void Reset() noexcept;

    std::optional<MasterFramePts> last_pts_ns() const noexcept;

private:
    MasterFrameId next_frame_id_ = 0;
    std::optional<MasterFramePts> last_pts_ns_;
    bool frame_id_exhausted_ = false;
};

// Retains the currently configured OBS frame interval independently from the
// master identity stream, so a live FPS change cannot rebase that stream.
class MasterFrameTimingConfiguration final {
public:
    MasterFrameTimingConfigurationResult ObserveFrameInterval(uint64_t frame_interval_ns) noexcept;
    void Reset() noexcept;

    std::optional<uint64_t> frame_interval_ns() const noexcept;

private:
    std::optional<uint64_t> frame_interval_ns_;
};

} // namespace detail

} // namespace obs_sync_replay
