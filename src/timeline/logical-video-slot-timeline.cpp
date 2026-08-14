#include "timeline/logical-video-slot-timeline.hpp"

#include <limits>

namespace obs_sync_replay::detail {

void LogicalVideoSlotTimeline::Reset() noexcept {
    previous_rendered_frame_.reset();
    next_slot_id_ = 0;
}

LogicalVideoSlotObservationResult
LogicalVideoSlotTimeline::ObserveRenderedFrame(const MasterFrame& rendered_frame,
                                               const uint64_t frame_interval_ns,
                                               std::vector<LogicalVideoSlot>& observed_slots) {
    observed_slots.clear();
    if (frame_interval_ns == 0) {
        return LogicalVideoSlotObservationResult::InvalidFrameInterval;
    }

    if (!previous_rendered_frame_.has_value()) {
        observed_slots.push_back({next_slot_id_++, rendered_frame.pts_ns(),
                                  rendered_frame.frame_id(), rendered_frame.pts_ns(),
                                  LogicalVideoSlotDisposition::Rendered});
        previous_rendered_frame_ = rendered_frame;
        return LogicalVideoSlotObservationResult::Accepted;
    }

    const MasterFrame& previous_rendered_frame = *previous_rendered_frame_;
    if (rendered_frame.pts_ns() <= previous_rendered_frame.pts_ns()) {
        return LogicalVideoSlotObservationResult::NonMonotonicRenderedPts;
    }

    const uint64_t delta_ns = rendered_frame.pts_ns() - previous_rendered_frame.pts_ns();
    if (delta_ns % frame_interval_ns != 0) {
        return LogicalVideoSlotObservationResult::UnalignedRenderedPts;
    }

    const uint64_t elapsed_slots = delta_ns / frame_interval_ns;
    if (elapsed_slots == 0 ||
        elapsed_slots >= std::numeric_limits<LogicalVideoSlotId>::max() - next_slot_id_) {
        return LogicalVideoSlotObservationResult::UnalignedRenderedPts;
    }

    for (uint64_t offset = 1; offset < elapsed_slots; ++offset) {
        observed_slots.push_back(
            {next_slot_id_++, previous_rendered_frame.pts_ns() + offset * frame_interval_ns,
             previous_rendered_frame.frame_id(), previous_rendered_frame.pts_ns(),
             LogicalVideoSlotDisposition::Repeated});
    }
    observed_slots.push_back({next_slot_id_++, rendered_frame.pts_ns(), rendered_frame.frame_id(),
                              rendered_frame.pts_ns(), LogicalVideoSlotDisposition::Rendered});
    previous_rendered_frame_ = rendered_frame;
    return LogicalVideoSlotObservationResult::Accepted;
}

} // namespace obs_sync_replay::detail
