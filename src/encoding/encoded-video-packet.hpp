#pragma once

#include "rendering/scene-renderer.hpp"
#include "timeline/master-frame.hpp"

#include <cstdint>
#include <vector>

namespace obs_sync_replay {

// Packet bytes are copied out of the NVENC lock before the lock is released.
// This object is independent of both NVENC and OBS packet storage.
struct EncodedVideoPacket final {
    MasterFrame master_frame;
    OutputSlot output;
    int64_t pts;
    int64_t dts;
    bool keyframe;
    std::vector<uint8_t> bytes;
};

} // namespace obs_sync_replay
