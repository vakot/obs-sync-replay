#pragma once

#include "recording/packet-sink.hpp"

#include <string>
#include <vector>

namespace obs_sync_replay {

struct ObsAudioConfiguration final {
    bool valid = true;
    std::string error;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    std::vector<AudioStreamConfig> recording_tracks;
};

// Reads the same profile keys OBS uses for its normal Recording/Replay audio
// encoders. The returned order is ascending mixer index, which is also OBS's
// mux-track order for enabled recording tracks.
ObsAudioConfiguration ReadObsAudioConfiguration() noexcept;

} // namespace obs_sync_replay
