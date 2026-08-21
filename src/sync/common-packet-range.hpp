#pragma once

#include "recording/encoded-packet-buffer.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace obs_sync_replay {

struct CommonPacketRange final {
    uint64_t start_cts = 0;
    uint64_t end_cts = 0;
};

enum class CommonPacketRangeFailure : uint8_t {
    None,
    NoCommonStartKeyframe,
    NoCommonEnd,
    MissingPacket,
    InvalidPacketOrdering,
};

struct CommonPacketRangeResult final {
    std::optional<CommonPacketRange> range;
    CommonPacketRangeFailure failure = CommonPacketRangeFailure::None;
    uint64_t mismatch_count = 0;
};

CommonPacketRangeResult SelectCommonStart(const EncodedPacketBuffer& a, const EncodedPacketBuffer& b,
                                          uint64_t requested_start_cts);
CommonPacketRangeResult SelectCommonEnd(const EncodedPacketBuffer& a, const EncodedPacketBuffer& b,
                                        CommonPacketRange range, uint64_t requested_stop_cts);
std::optional<uint64_t> SelectCommonPrefixEnd(const EncodedPacketBuffer& a, const EncodedPacketBuffer& b,
                                              uint64_t start_cts, uint64_t upper_cts,
                                              uint64_t expected_cts_step = 0);
CommonPacketRangeResult ValidateCommonPacketRange(const EncodedPacketBuffer& a, const EncodedPacketBuffer& b,
                                                  CommonPacketRange range);

std::vector<EncodedPacket> SelectPackets(const EncodedPacketBuffer& buffer, CommonPacketRange range);
std::vector<EncodedPacket> SortForDecodeOrder(std::vector<EncodedPacket> packets);

const char* CommonPacketRangeFailureName(CommonPacketRangeFailure failure) noexcept;

} // namespace obs_sync_replay
