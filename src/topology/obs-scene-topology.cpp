#include "topology/obs-scene-topology.hpp"

#include <obs-module.h>

#include <utility>

namespace obs_sync_replay {

DiscoveredObsScene::~DiscoveredObsScene() {
    if (source) {
        obs_source_release(source);
    }
}

DiscoveredObsScene::DiscoveredObsScene(DiscoveredObsScene&& other) noexcept
    : scene(std::move(other.scene)), source(std::exchange(other.source, nullptr)) {}

DiscoveredObsScene& DiscoveredObsScene::operator=(DiscoveredObsScene&& other) noexcept {
    if (this != &other) {
        if (source) {
            obs_source_release(source);
        }
        scene = std::move(other.scene);
        source = std::exchange(other.source, nullptr);
    }
    return *this;
}

namespace {

bool CollectScene(void* param, obs_source_t* source) {
    auto* scenes = static_cast<std::vector<DiscoveredObsScene>*>(param);
    const char* uuid = obs_source_get_uuid(source);
    const char* name = obs_source_get_name(source);
    if (!uuid || !*uuid || !name || !*name) {
        blog(LOG_WARNING, "[topology] scene-skipped reason=missing-public-uuid-or-name");
        return true;
    }
    obs_source_t* retained = obs_source_get_ref(source);
    if (!retained) {
        blog(LOG_ERROR, "[topology] scene-skipped uuid=%s reason=source-reference-failed", uuid);
        return true;
    }
    DiscoveredObsScene discovered;
    discovered.scene = {uuid, name};
    discovered.source = retained;
    scenes->push_back(std::move(discovered));
    return true;
}

} // namespace

std::vector<DiscoveredObsScene> DiscoverObsScenes() {
    std::vector<DiscoveredObsScene> scenes;
    obs_enum_scenes(CollectScene, &scenes);
    return scenes;
}

} // namespace obs_sync_replay
