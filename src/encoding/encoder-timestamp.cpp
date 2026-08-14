#include "encoding/encoder-timestamp.hpp"

#include <limits>

namespace obs_sync_replay {

std::optional<int64_t> MasterPtsToEncoderPts(const MasterFrame& master_frame) noexcept {
    if (master_frame.pts_ns() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return std::nullopt;
    }

    return static_cast<int64_t>(master_frame.pts_ns());
}

} // namespace obs_sync_replay
