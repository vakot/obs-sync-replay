#include <obs-module.h>

#include "timeline/master-frame-coordinator/master-frame-coordinator.hpp"

#include <memory>

namespace {

std::unique_ptr<obs_sync_replay::MasterFrameCoordinator> master_frame_coordinator;

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
    master_frame_coordinator = std::make_unique<obs_sync_replay::MasterFrameCoordinator>(
        [](const obs_sync_replay::MasterFrame &) {});
    master_frame_coordinator->Start();
    return true;
}

void obs_module_unload(void) {
    master_frame_coordinator.reset();
    blog(LOG_INFO, "[obs-sync-replay] plugin unloaded");
}
