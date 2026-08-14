#include <obs-module.h>

#include "rendering/synchronized-scene-renderer.hpp"
#include "timeline/master-frame-coordinator.hpp"

#include <memory>

namespace {

std::unique_ptr<obs_sync_replay::MasterFrameCoordinator> master_frame_coordinator;
std::unique_ptr<obs_sync_replay::SynchronizedSceneRenderer> synchronized_scene_renderer;

constexpr char kDevelopmentSceneA[] = "Gameplay Test";
constexpr char kDevelopmentSceneB[] = "Camera Test";

} // namespace

OBS_DECLARE_MODULE()

MODULE_EXPORT const char* obs_module_name(void) {
    return "OBS Sync Replay";
}

MODULE_EXPORT const char* obs_module_description(void) {
    return "Frame-perfect synchronized replay for two OBS scenes";
}

bool obs_module_load(void) {
    blog(LOG_INFO, "[obs-sync-replay] plugin loaded (version %s)", OBS_SYNC_REPLAY_VERSION);
    synchronized_scene_renderer =
        std::make_unique<obs_sync_replay::SynchronizedSceneRenderer>(kDevelopmentSceneA, kDevelopmentSceneB);
    master_frame_coordinator = std::make_unique<obs_sync_replay::MasterFrameCoordinator>(
        [](const obs_sync_replay::MasterFrame &frame) { synchronized_scene_renderer->Render(frame); });
    master_frame_coordinator->Start();
    return true;
}

void obs_module_unload(void) {
    master_frame_coordinator.reset();
    synchronized_scene_renderer.reset();
    blog(LOG_INFO, "[obs-sync-replay] plugin unloaded");
}
