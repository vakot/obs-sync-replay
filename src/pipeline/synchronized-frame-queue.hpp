#pragma once

#include "timeline/master-frame.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace obs_sync_replay {

// Metadata submitted with both GPU resources. The queue accepts only a complete
// pair with equal identity; it has no independently advancing output queues.
struct SynchronizedFramePairIdentity final {
    MasterFrame output_a;
    MasterFrame output_b;
    bool resources_complete;
};

enum class SynchronizedFrameQueueResult : uint8_t {
    Retained,
    InvalidPair,
    Capacity,
};

// OBS-independent fixed-capacity FIFO for the one synchronized pair stream.
class SynchronizedFrameQueue final {
  public:
    explicit SynchronizedFrameQueue(size_t capacity);

    SynchronizedFrameQueueResult
    CanRetain(const SynchronizedFramePairIdentity& pair) const noexcept;
    SynchronizedFrameQueueResult TryRetain(const SynchronizedFramePairIdentity& pair);
    std::optional<MasterFrame> TakeNext();
    void Reset() noexcept;

    size_t size() const noexcept;
    size_t capacity() const noexcept;

  private:
    static bool IsValid(const SynchronizedFramePairIdentity& pair) noexcept;
    void CompactIfDrained() noexcept;

    size_t capacity_;
    size_t head_ = 0;
    std::vector<MasterFrame> frames_;
};

const char* SynchronizedFrameQueueResultName(SynchronizedFrameQueueResult result) noexcept;

} // namespace obs_sync_replay
