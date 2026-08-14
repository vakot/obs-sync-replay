#pragma once

#include <cstdint>

namespace obs_sync_replay {

using MasterFrameId = uint64_t;
using MasterFramePts = uint64_t;

namespace detail {
class MasterFrameTimeline;
}

// Immutable temporal identity issued only by the internal master timeline.
class MasterFrame final {
public:
    MasterFrameId frame_id() const noexcept {
        return frame_id_;
    }

    MasterFramePts pts_ns() const noexcept {
        return pts_ns_;
    }

private:
    friend class detail::MasterFrameTimeline;

    MasterFrame(const MasterFrameId frame_id, const MasterFramePts pts_ns) noexcept
        : frame_id_(frame_id), pts_ns_(pts_ns) {}

    MasterFrameId frame_id_;
    MasterFramePts pts_ns_;
};

} // namespace obs_sync_replay
