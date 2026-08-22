#pragma once

#include "config/replay-configuration.hpp"
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
    ReplayConfiguration replay;
    std::vector<ConfiguredStream> streams;
};

bool StreamParticipates(StreamParticipationMode mode, CaptureConsumer consumer) noexcept;
bool StreamNeedsEncoder(StreamParticipationMode mode, bool recording_active, bool replay_active) noexcept;
const char* StreamParticipationModeName(StreamParticipationMode mode) noexcept;
const char* StreamIdentityName(StreamIdentity identity) noexcept;

std::vector<size_t> SelectedStreamIndexes(const CaptureConfiguration& configuration,
                                          CaptureConsumer consumer);

} // namespace obs_sync_replay
