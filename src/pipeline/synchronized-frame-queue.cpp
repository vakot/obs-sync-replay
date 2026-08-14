#include "pipeline/synchronized-frame-queue.hpp"

#include <stdexcept>

namespace obs_sync_replay {

SynchronizedFrameQueue::SynchronizedFrameQueue(const size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("synchronized frame queue capacity must be nonzero");
    }

    frames_.reserve(capacity_);
}

SynchronizedFrameQueueResult
SynchronizedFrameQueue::CanRetain(const SynchronizedFramePairIdentity& pair) const noexcept {
    if (!IsValid(pair)) {
        return SynchronizedFrameQueueResult::InvalidPair;
    }
    return size() >= capacity_ ? SynchronizedFrameQueueResult::Capacity
                               : SynchronizedFrameQueueResult::Retained;
}

SynchronizedFrameQueueResult
SynchronizedFrameQueue::TryRetain(const SynchronizedFramePairIdentity& pair) {
    const SynchronizedFrameQueueResult result = CanRetain(pair);
    if (result != SynchronizedFrameQueueResult::Retained) {
        return result;
    }

    CompactIfDrained();
    frames_.push_back(pair.output_a);
    return SynchronizedFrameQueueResult::Retained;
}

std::optional<MasterFrame> SynchronizedFrameQueue::TakeNext() {
    if (head_ == frames_.size()) {
        return std::nullopt;
    }

    const MasterFrame frame = frames_[head_++];
    CompactIfDrained();
    return frame;
}

void SynchronizedFrameQueue::Reset() noexcept {
    frames_.clear();
    head_ = 0;
}

size_t SynchronizedFrameQueue::size() const noexcept {
    return frames_.size() - head_;
}

size_t SynchronizedFrameQueue::capacity() const noexcept {
    return capacity_;
}

bool SynchronizedFrameQueue::IsValid(const SynchronizedFramePairIdentity& pair) noexcept {
    return pair.resources_complete && pair.output_a.frame_id() == pair.output_b.frame_id() &&
           pair.output_a.pts_ns() == pair.output_b.pts_ns();
}

void SynchronizedFrameQueue::CompactIfDrained() noexcept {
    if (head_ == frames_.size()) {
        frames_.clear();
        head_ = 0;
    }
}

const char* SynchronizedFrameQueueResultName(const SynchronizedFrameQueueResult result) noexcept {
    switch (result) {
    case SynchronizedFrameQueueResult::Retained:
        return "retained";
    case SynchronizedFrameQueueResult::InvalidPair:
        return "invalid-pair";
    case SynchronizedFrameQueueResult::Capacity:
        return "capacity";
    }

    return "unknown";
}

} // namespace obs_sync_replay
