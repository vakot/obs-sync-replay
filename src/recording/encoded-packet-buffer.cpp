#include "recording/encoded-packet-buffer.hpp"

#include <utility>

namespace obs_sync_replay {

EncodedPacketBuffer::EncodedPacketBuffer(const size_t capacity_bytes) : capacity_bytes_(capacity_bytes) {}

EncodedPacketBufferResult EncodedPacketBuffer::Push(EncodedPacket packet) {
    if (packet.timebase_num <= 0 || packet.timebase_den <= 0) {
        return EncodedPacketBufferResult::MissingTiming;
    }
    if (packets_.find(packet.source_cts) != packets_.end()) {
        return EncodedPacketBufferResult::DuplicateSourceCts;
    }
    if (packet.payload.size() > capacity_bytes_ - (bytes_ <= capacity_bytes_ ? bytes_ : capacity_bytes_)) {
        return EncodedPacketBufferResult::Capacity;
    }

    bytes_ += packet.payload.size();
    peak_bytes_ = bytes_ > peak_bytes_ ? bytes_ : peak_bytes_;
    packets_.emplace(packet.source_cts, std::move(packet));
    return EncodedPacketBufferResult::Retained;
}

void EncodedPacketBuffer::Clear() noexcept {
    packets_.clear();
    bytes_ = 0;
}

void EncodedPacketBuffer::DiscardBefore(const uint64_t source_cts) {
    auto it = packets_.begin();
    while (it != packets_.end() && it->first < source_cts) {
        bytes_ -= it->second.payload.size();
        it = packets_.erase(it);
    }
}

bool EncodedPacketBuffer::Contains(const uint64_t source_cts) const noexcept {
    return packets_.find(source_cts) != packets_.end();
}

const EncodedPacket* EncodedPacketBuffer::Find(const uint64_t source_cts) const noexcept {
    const auto it = packets_.find(source_cts);
    return it == packets_.end() ? nullptr : &it->second;
}

std::vector<EncodedPacket> EncodedPacketBuffer::Snapshot() const {
    std::vector<EncodedPacket> result;
    result.reserve(packets_.size());
    for (const auto& [source_cts, packet] : packets_) {
        (void)source_cts;
        result.push_back(packet);
    }
    return result;
}

std::vector<EncodedPacket> EncodedPacketBuffer::SnapshotThrough(const uint64_t source_cts) const {
    std::vector<EncodedPacket> result;
    for (const auto& [packet_cts, packet] : packets_) {
        if (packet_cts > source_cts) {
            break;
        }
        result.push_back(packet);
    }
    return result;
}

size_t EncodedPacketBuffer::size() const noexcept {
    return packets_.size();
}

size_t EncodedPacketBuffer::bytes() const noexcept {
    return bytes_;
}

size_t EncodedPacketBuffer::peak_bytes() const noexcept {
    return peak_bytes_;
}

size_t EncodedPacketBuffer::capacity_bytes() const noexcept {
    return capacity_bytes_;
}

bool EncodedPacketBuffer::SetCapacityBytes(const size_t capacity_bytes) noexcept {
    if (capacity_bytes < bytes_) {
        return false;
    }
    capacity_bytes_ = capacity_bytes;
    return true;
}

} // namespace obs_sync_replay
