#include "control/capture-configuration.hpp"

namespace obs_sync_replay {

bool StreamParticipates(const StreamParticipationMode mode, const CaptureConsumer consumer) noexcept {
    switch (mode) {
    case StreamParticipationMode::Disabled:
        return false;
    case StreamParticipationMode::Recording:
        return consumer == CaptureConsumer::Recording;
    case StreamParticipationMode::Replay:
        return consumer == CaptureConsumer::Replay;
    case StreamParticipationMode::Both:
        return true;
    }
    return false;
}

bool StreamNeedsEncoder(const StreamParticipationMode mode, const bool recording_active,
                        const bool replay_active) noexcept {
    return (recording_active && StreamParticipates(mode, CaptureConsumer::Recording)) ||
           (replay_active && StreamParticipates(mode, CaptureConsumer::Replay));
}

const char* StreamParticipationModeName(const StreamParticipationMode mode) noexcept {
    switch (mode) {
    case StreamParticipationMode::Disabled:
        return "disabled";
    case StreamParticipationMode::Recording:
        return "recording";
    case StreamParticipationMode::Replay:
        return "replay";
    case StreamParticipationMode::Both:
        return "both";
    }
    return "unknown";
}

std::string StreamIdentityName(const StreamIdentity& identity) {
    return StreamIdentityLabel(identity);
}

std::vector<size_t> SelectedStreamIndexes(const CaptureConfiguration& configuration,
                                          const CaptureConsumer consumer) {
    std::vector<size_t> indexes;
    for (size_t index = 0; index < configuration.streams.size(); ++index) {
        if (StreamParticipates(configuration.streams[index].mode, consumer)) {
            indexes.push_back(index);
        }
    }
    return indexes;
}

} // namespace obs_sync_replay
