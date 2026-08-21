#include "sync/common-packet-range.hpp"

#include <algorithm>

namespace obs_sync_replay {

namespace {

std::vector<uint64_t> SourceCts(const EncodedPacketBuffer& buffer, const CommonPacketRange range) {
    std::vector<uint64_t> result;
    for (const EncodedPacket& packet : buffer.Snapshot()) {
        if (packet.source_cts >= range.start_cts && packet.source_cts <= range.end_cts) {
            result.push_back(packet.source_cts);
        }
    }
    return result;
}

bool HasValidDecodeOrder(const std::vector<EncodedPacket>& packets) {
    const std::vector<EncodedPacket> sorted = SortForDecodeOrder(packets);
    for (size_t index = 1; index < sorted.size(); ++index) {
        if (sorted[index - 1].dts > sorted[index].dts) {
            return false;
        }
    }
    return true;
}

} // namespace

CommonPacketRangeResult SelectCommonStart(const EncodedPacketBuffer& a, const EncodedPacketBuffer& b,
                                          const uint64_t requested_start_cts) {
    for (const EncodedPacket& packet : a.Snapshot()) {
        if (packet.source_cts >= requested_start_cts && packet.keyframe) {
            const EncodedPacket* other = b.Find(packet.source_cts);
            if (other && other->keyframe) {
                return {{CommonPacketRange{packet.source_cts, packet.source_cts}}, CommonPacketRangeFailure::None, 0};
            }
        }
    }
    return {std::nullopt, CommonPacketRangeFailure::NoCommonStartKeyframe, 0};
}

CommonPacketRangeResult SelectCommonEnd(const EncodedPacketBuffer& a, const EncodedPacketBuffer& b,
                                        const CommonPacketRange range, const uint64_t requested_stop_cts) {
    const std::optional<uint64_t> candidate =
        SelectCommonPrefixEnd(a, b, range.start_cts, requested_stop_cts);
    if (!candidate) {
        return {std::nullopt, CommonPacketRangeFailure::NoCommonEnd, 0};
    }
    return {{CommonPacketRange{range.start_cts, *candidate}}, CommonPacketRangeFailure::None, 0};
}

std::optional<uint64_t> SelectCommonPrefixEnd(const EncodedPacketBuffer& a, const EncodedPacketBuffer& b,
                                              const uint64_t start_cts, const uint64_t upper_cts,
                                              const uint64_t expected_cts_step) {
    const std::vector<EncodedPacket> packets_a = a.Snapshot();
    const std::vector<EncodedPacket> packets_b = b.Snapshot();
    size_t index_a = 0;
    size_t index_b = 0;
    while (index_a < packets_a.size() && packets_a[index_a].source_cts < start_cts) {
        ++index_a;
    }
    while (index_b < packets_b.size() && packets_b[index_b].source_cts < start_cts) {
        ++index_b;
    }

    std::optional<uint64_t> result;
    while (index_a < packets_a.size() && index_b < packets_b.size()) {
        const uint64_t cts_a = packets_a[index_a].source_cts;
        const uint64_t cts_b = packets_b[index_b].source_cts;
        if (cts_a > upper_cts || cts_b > upper_cts || cts_a != cts_b) {
            break;
        }
        if (result && expected_cts_step != 0) {
            const uint64_t delta = cts_a - *result;
            const uint64_t difference = delta > expected_cts_step ? delta - expected_cts_step
                                                                   : expected_cts_step - delta;
            if (difference > 1) {
                break;
            }
        }
        result = cts_a;
        ++index_a;
        ++index_b;
    }
    return result;
}

CommonPacketRangeResult ValidateCommonPacketRange(const EncodedPacketBuffer& a, const EncodedPacketBuffer& b,
                                                  const CommonPacketRange range) {
    const std::vector<uint64_t> source_a = SourceCts(a, range);
    const std::vector<uint64_t> source_b = SourceCts(b, range);
    uint64_t mismatches = 0;
    for (const uint64_t source_cts : source_a) {
        if (!b.Contains(source_cts)) {
            ++mismatches;
        }
    }
    for (const uint64_t source_cts : source_b) {
        if (!a.Contains(source_cts)) {
            ++mismatches;
        }
    }
    if (mismatches != 0) {
        return {std::nullopt, CommonPacketRangeFailure::MissingPacket, mismatches};
    }

    const std::vector<EncodedPacket> packets_a = SelectPackets(a, range);
    const std::vector<EncodedPacket> packets_b = SelectPackets(b, range);
    if (!HasValidDecodeOrder(packets_a) || !HasValidDecodeOrder(packets_b)) {
        return {std::nullopt, CommonPacketRangeFailure::InvalidPacketOrdering, 0};
    }
    return {{range}, CommonPacketRangeFailure::None, 0};
}

std::vector<EncodedPacket> SelectPackets(const EncodedPacketBuffer& buffer, const CommonPacketRange range) {
    std::vector<EncodedPacket> result;
    for (EncodedPacket packet : buffer.Snapshot()) {
        if (packet.source_cts >= range.start_cts && packet.source_cts <= range.end_cts) {
            result.push_back(std::move(packet));
        }
    }
    return result;
}

std::vector<EncodedPacket> SortForDecodeOrder(std::vector<EncodedPacket> packets) {
    std::stable_sort(packets.begin(), packets.end(), [](const EncodedPacket& left, const EncodedPacket& right) {
        if (left.dts != right.dts) {
            return left.dts < right.dts;
        }
        return left.pts < right.pts;
    });
    return packets;
}

const char* CommonPacketRangeFailureName(const CommonPacketRangeFailure failure) noexcept {
    switch (failure) {
    case CommonPacketRangeFailure::None:
        return "none";
    case CommonPacketRangeFailure::NoCommonStartKeyframe:
        return "no-common-start-keyframe";
    case CommonPacketRangeFailure::NoCommonEnd:
        return "no-common-end";
    case CommonPacketRangeFailure::MissingPacket:
        return "missing-packet";
    case CommonPacketRangeFailure::InvalidPacketOrdering:
        return "invalid-packet-ordering";
    }
    return "unknown";
}

} // namespace obs_sync_replay
