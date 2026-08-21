#pragma once

#include "timeline/master-frame.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace obs_sync_replay::detail {

using LogicalVideoSlotId = uint64_t;

enum class LogicalVideoSlotDisposition {
    Rendered,
    Repeated,
};

struct LogicalVideoSlot final {
    LogicalVideoSlotId slot_id = 0;
    MasterFramePts pts_ns = 0;
    MasterFrameId rendered_frame_id = 0;
    MasterFramePts rendered_pts_ns = 0;
    LogicalVideoSlotDisposition disposition = LogicalVideoSlotDisposition::Rendered;
};

enum class LogicalVideoSlotObservationResult {
    Accepted,
    InvalidFrameInterval,
    NonMonotonicRenderedPts,
    UnalignedRenderedPts,
};

// Represents the logical slots libobs creates from a rendered graphics frame and
// obs_vframe_info.count. Repeated slots are OBS-owned temporal positions, not
// plugin-fabricated rendered frames.
class LogicalVideoSlotTimeline final {
  public:
    void Reset() noexcept;

    [[nodiscard]] LogicalVideoSlotObservationResult
    ObserveRenderedFrame(const MasterFrame& rendered_frame, uint64_t frame_interval_ns,
                         std::vector<LogicalVideoSlot>& observed_slots);

  private:
    std::optional<MasterFrame> previous_rendered_frame_;
    LogicalVideoSlotId next_slot_id_ = 0;
};

} // namespace obs_sync_replay::detail
