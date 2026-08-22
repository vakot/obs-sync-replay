#pragma once

#include <optional>
#include <string>
#include <vector>

namespace obs_sync_replay {

struct RegisteredAudioEncoder final {
    std::string id;
    std::string codec;
};

// Mirrors the stable part of OBS's frontend selection contract: an explicit
// registered ID is authoritative; a Simple-output codec alias resolves to a
// registered encoder for that codec, using OBS's platform preference order.
std::optional<std::string> ResolveAudioEncoderId(
    const std::string& configured, const std::vector<RegisteredAudioEncoder>& registered);

} // namespace obs_sync_replay
