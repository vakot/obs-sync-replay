#include "encoding/encoded-packet-tracker.hpp"

#include <stdexcept>

namespace obs_sync_replay {

EncodedPacketTracker::EncodedPacketTracker(const size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("encoded packet tracker capacity must be nonzero");
    }
}

EncodedPacketTrackerResult EncodedPacketTracker::Begin(const MasterFrame& master_frame,
                                                        const int64_t encoder_pts) {
    if (pending_.find(master_frame.frame_id()) != pending_.end()) {
        return EncodedPacketTrackerResult::Duplicate;
    }
    if (pending_.size() >= capacity_) {
        return EncodedPacketTrackerResult::Capacity;
    }

    pending_.emplace(master_frame.frame_id(), PendingPair{master_frame, encoder_pts});
    return EncodedPacketTrackerResult::Accepted;
}

EncodedPacketTrackerResult EncodedPacketTracker::Record(const EncodedVideoPacket& packet) noexcept {
    const auto pending = pending_.find(packet.master_frame.frame_id());
    if (pending == pending_.end()) {
        return EncodedPacketTrackerResult::UnknownMasterFrame;
    }

    PendingPair& pair = pending->second;
    if (pair.master_frame.pts_ns() != packet.master_frame.pts_ns() || pair.encoder_pts != packet.pts) {
        return EncodedPacketTrackerResult::TimestampMismatch;
    }

    bool& received = packet.output == OutputSlot::A ? pair.output_a_received : pair.output_b_received;
    if (received) {
        return EncodedPacketTrackerResult::Duplicate;
    }
    received = true;
    if (pair.output_a_received && pair.output_b_received) {
        pending_.erase(pending);
    }
    return EncodedPacketTrackerResult::Accepted;
}

void EncodedPacketTracker::Reset() noexcept {
    pending_.clear();
}

size_t EncodedPacketTracker::size() const noexcept {
    return pending_.size();
}

size_t EncodedPacketTracker::capacity() const noexcept {
    return capacity_;
}

const char* EncodedPacketTrackerResultName(const EncodedPacketTrackerResult result) noexcept {
    switch (result) {
    case EncodedPacketTrackerResult::Accepted:
        return "accepted";
    case EncodedPacketTrackerResult::Duplicate:
        return "duplicate";
    case EncodedPacketTrackerResult::UnknownMasterFrame:
        return "unknown-master-frame";
    case EncodedPacketTrackerResult::WrongOutput:
        return "wrong-output";
    case EncodedPacketTrackerResult::TimestampMismatch:
        return "timestamp-mismatch";
    case EncodedPacketTrackerResult::Capacity:
        return "capacity";
    }

    return "unknown";
}

} // namespace obs_sync_replay
