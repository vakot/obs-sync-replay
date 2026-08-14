#pragma once

#include "encoding/encoded-video-packet.hpp"

#include <cstddef>
#include <cstdint>
#include <map>

namespace obs_sync_replay {

enum class EncodedPacketTrackerResult : uint8_t {
    Accepted,
    Duplicate,
    UnknownMasterFrame,
    WrongOutput,
    TimestampMismatch,
    Capacity,
};

// OBS-independent bounded association table. It records identities before any
// encoder work begins, so completion order cannot associate A with B.
class EncodedPacketTracker final {
  public:
    explicit EncodedPacketTracker(size_t capacity);

    EncodedPacketTrackerResult Begin(const MasterFrame& master_frame, int64_t encoder_pts);
    EncodedPacketTrackerResult Record(const EncodedVideoPacket& packet) noexcept;
    void Reset() noexcept;

    size_t size() const noexcept;
    size_t capacity() const noexcept;

  private:
    struct PendingPair final {
        MasterFrame master_frame;
        int64_t encoder_pts;
        bool output_a_received = false;
        bool output_b_received = false;
    };

    size_t capacity_;
    std::map<MasterFrameId, PendingPair> pending_;
};

const char* EncodedPacketTrackerResultName(EncodedPacketTrackerResult result) noexcept;

} // namespace obs_sync_replay
