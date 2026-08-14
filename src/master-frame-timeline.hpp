#pragma once

#include <cstdint>
#include <optional>

namespace obs_sync_replay {

using MasterFrameId = uint64_t;
using MasterFramePts = uint64_t;

namespace detail {
class MasterFrameTimeline;
}

// Immutable temporal identity issued only by MasterFrameTimeline.
class MasterFrame final {
public:
    MasterFrameId frame_id() const noexcept;
    MasterFramePts pts_ns() const noexcept;

private:
    friend class detail::MasterFrameTimeline;

    MasterFrame(MasterFrameId frame_id, MasterFramePts pts_ns) noexcept;

    MasterFrameId frame_id_;
    MasterFramePts pts_ns_;
};

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
