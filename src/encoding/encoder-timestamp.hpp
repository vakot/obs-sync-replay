#pragma once

#include "timeline/master-frame.hpp"

#include <cstdint>
#include <optional>

namespace obs_sync_replay {

// Direct NVENC returns inputTimeStamp verbatim. Keeping the encoder clock in
// nanoseconds avoids cadence-dependent rescaling of the canonical OBS PTS.
struct EncoderTimebase final {
    int32_t numerator = 1;
    int32_t denominator = 1000000000;
};

constexpr EncoderTimebase kEncoderTimebase{};

std::optional<int64_t> MasterPtsToEncoderPts(const MasterFrame& master_frame) noexcept;

} // namespace obs_sync_replay
