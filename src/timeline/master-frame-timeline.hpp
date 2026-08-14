#pragma once

#include "timeline/master-frame.hpp"

#include <optional>

namespace obs_sync_replay {

enum class MasterFrameObservationResult {
    Accepted,
    NonMonotonicPts,
    FrameIdExhausted,
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

} // namespace detail

} // namespace obs_sync_replay
