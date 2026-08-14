#include "timeline/master-frame-timeline/master-frame-timeline.hpp"

#include <limits>

namespace obs_sync_replay {

namespace detail {

MasterFrameObservationResult MasterFrameTimeline::Observe(const MasterFramePts pts_ns,
                                                           std::optional<MasterFrame> &frame) noexcept {
    if (frame_id_exhausted_) {
        return MasterFrameObservationResult::FrameIdExhausted;
    }

    if (last_pts_ns_.has_value() && pts_ns <= *last_pts_ns_) {
        return MasterFrameObservationResult::NonMonotonicPts;
    }

    frame = MasterFrame(next_frame_id_, pts_ns);
    last_pts_ns_ = pts_ns;

    if (next_frame_id_ == std::numeric_limits<MasterFrameId>::max()) {
        frame_id_exhausted_ = true;
    } else {
        ++next_frame_id_;
    }

    return MasterFrameObservationResult::Accepted;
}

void MasterFrameTimeline::Reset() noexcept {
    next_frame_id_ = 0;
    last_pts_ns_.reset();
    frame_id_exhausted_ = false;
}

std::optional<MasterFramePts> MasterFrameTimeline::last_pts_ns() const noexcept {
    return last_pts_ns_;
}

MasterFrameTimingConfigurationResult MasterFrameTimingConfiguration::ObserveFrameInterval(
    const uint64_t frame_interval_ns) noexcept {
    if (frame_interval_ns == 0) {
        return MasterFrameTimingConfigurationResult::InvalidInterval;
    }

    if (!frame_interval_ns_.has_value()) {
        frame_interval_ns_ = frame_interval_ns;
        return MasterFrameTimingConfigurationResult::Initialized;
    }

    if (*frame_interval_ns_ == frame_interval_ns) {
        return MasterFrameTimingConfigurationResult::Unchanged;
    }

    frame_interval_ns_ = frame_interval_ns;
    return MasterFrameTimingConfigurationResult::Changed;
}

void MasterFrameTimingConfiguration::Reset() noexcept {
    frame_interval_ns_.reset();
}

std::optional<uint64_t> MasterFrameTimingConfiguration::frame_interval_ns() const noexcept {
    return frame_interval_ns_;
}

} // namespace detail

} // namespace obs_sync_replay
