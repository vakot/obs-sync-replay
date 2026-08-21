#pragma once

#include "recording/encoded-packet-buffer.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace obs_sync_replay {

struct PacketStreamConfig final {
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t timebase_num = 0;
    int32_t timebase_den = 0;
    std::vector<uint8_t> extra_data;
};

class SynchronizedPacketSink {
  public:
    virtual ~SynchronizedPacketSink() = default;
    virtual bool Open(const PacketStreamConfig& config, uint64_t common_start_cts) = 0;
    virtual bool Write(const EncodedPacket& packet) = 0;
    virtual bool CommitThrough(uint64_t source_cts) = 0;
    virtual bool Finalize(uint64_t common_end_cts) = 0;
    virtual void Abort() noexcept = 0;
};

} // namespace obs_sync_replay
