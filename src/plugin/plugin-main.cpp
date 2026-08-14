#include <obs-module.h>

#include "encoding/native-obs-encoder-experiment.hpp"
#include "timeline/master-frame-coordinator.hpp"

#include <memory>

namespace {

std::unique_ptr<obs_sync_replay::MasterFrameCoordinator> master_frame_coordinator;
std::unique_ptr<obs_sync_replay::NativeObsEncoderExperiment> native_encoder_experiment;

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
    native_encoder_experiment = std::make_unique<obs_sync_replay::NativeObsEncoderExperiment>(
        kDevelopmentSceneA, kDevelopmentSceneB);
    master_frame_coordinator = std::make_unique<obs_sync_replay::MasterFrameCoordinator>(
        [](const obs_sync_replay::MasterFrame& frame) {
            native_encoder_experiment->ObserveMasterFrame(frame);
        });
    master_frame_coordinator->Start();
    return true;
}

void obs_module_unload(void) {
    master_frame_coordinator.reset();
    native_encoder_experiment.reset();
    blog(LOG_INFO, "[obs-sync-replay] plugin unloaded");
}
