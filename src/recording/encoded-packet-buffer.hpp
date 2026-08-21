#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace obs_sync_replay {

struct EncodedPacket final {
    uint64_t source_cts = 0;
    int64_t pts = 0;
    int64_t dts = 0;
    int32_t timebase_num = 0;
    int32_t timebase_den = 0;
    bool keyframe = false;
    std::vector<uint8_t> payload;
};

enum class EncodedPacketBufferResult : uint8_t {
    Retained,
    MissingTiming,
    DuplicateSourceCts,
    Capacity,
};

// Owns compressed packet bytes and their source-timeline metadata. It never
// infers identity from callback order and never evicts a packet silently.
class EncodedPacketBuffer final {
  public:
    explicit EncodedPacketBuffer(size_t capacity_bytes);

    EncodedPacketBufferResult Push(EncodedPacket packet);
    void Clear() noexcept;
    void DiscardBefore(uint64_t source_cts);

    bool Contains(uint64_t source_cts) const noexcept;
    const EncodedPacket* Find(uint64_t source_cts) const noexcept;
    std::vector<EncodedPacket> Snapshot() const;
    std::vector<EncodedPacket> SnapshotThrough(uint64_t source_cts) const;

    size_t size() const noexcept;
    size_t bytes() const noexcept;
    size_t peak_bytes() const noexcept;
    size_t capacity_bytes() const noexcept;
    bool SetCapacityBytes(size_t capacity_bytes) noexcept;

  private:
    size_t capacity_bytes_;
    size_t bytes_ = 0;
    size_t peak_bytes_ = 0;
    std::map<uint64_t, EncodedPacket> packets_;
};

} // namespace obs_sync_replay
