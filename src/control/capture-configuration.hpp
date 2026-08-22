#pragma once

#include "capture/synchronized-capture-session.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace obs_sync_replay {

enum class StreamIdentity : uint8_t {
    Master,
    SceneA,
    SceneB,
};

enum class StreamParticipationMode : uint8_t {
    Disabled,
    Recording,
    Replay,
    Both,
};

enum class CaptureConsumer : uint8_t {
    Recording,
    Replay,
};

struct ConfiguredStream final {
    StreamIdentity identity = StreamIdentity::Master;
    std::string name;
    StreamParticipationMode mode = StreamParticipationMode::Both;
    PacketStreamConfig packet_config;
};

struct CaptureConfiguration final {
    uint64_t replay_duration_ns = 8'000'000'000ULL;
    size_t ring_capacity_bytes = 30 * 1024 * 1024;
    std::vector<ConfiguredStream> streams;
};

bool StreamParticipates(StreamParticipationMode mode, CaptureConsumer consumer) noexcept;
bool StreamNeedsEncoder(StreamParticipationMode mode, bool recording_active, bool replay_active) noexcept;
const char* StreamParticipationModeName(StreamParticipationMode mode) noexcept;
const char* StreamIdentityName(StreamIdentity identity) noexcept;

std::vector<size_t> SelectedStreamIndexes(const CaptureConfiguration& configuration,
                                          CaptureConsumer consumer);

} // namespace obs_sync_replay
