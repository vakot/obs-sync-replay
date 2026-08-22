#pragma once

#include "topology/scene-topology.hpp"

#include <obs.h>

#include <vector>

namespace obs_sync_replay {

struct DiscoveredObsScene final {
    DiscoveredScene scene;
    obs_source_t* source = nullptr;

    DiscoveredObsScene() = default;
    ~DiscoveredObsScene();
    DiscoveredObsScene(const DiscoveredObsScene&) = delete;
    DiscoveredObsScene& operator=(const DiscoveredObsScene&) = delete;
    DiscoveredObsScene(DiscoveredObsScene&& other) noexcept;
    DiscoveredObsScene& operator=(DiscoveredObsScene&& other) noexcept;
};

std::vector<DiscoveredObsScene> DiscoverObsScenes();

} // namespace obs_sync_replay
