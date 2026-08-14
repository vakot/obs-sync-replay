#include <obs-module.h>
#include <obs-frontend-api.h>

#include "encoding/native-obs-encoder-experiment.hpp"
#include "timeline/master-frame-coordinator.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

std::unique_ptr<obs_sync_replay::MasterFrameCoordinator> master_frame_coordinator;
std::unique_ptr<obs_sync_replay::NativeObsEncoderExperiment> native_encoder_experiment;
bool native_encoder_experiment_autostart_enabled = false;
bool native_encoder_experiment_frontend_callback_registered = false;

constexpr char kDevelopmentSceneA[] = "Gameplay Test";
constexpr char kDevelopmentSceneB[] = "Camera Test";

bool ExperimentalAutostartEnabled() {
    char* value = nullptr;
    size_t value_length = 0;
    const int result = _dupenv_s(&value, &value_length, "OBS_SYNC_REPLAY_EXPERIMENT_AUTOSTART");
    const bool enabled = result == 0 && value_length > 1 && std::strcmp(value, "1") == 0;
    std::free(value);
    return enabled;
}

void OnFrontendEvent(const obs_frontend_event event, void*) {
    if (!native_encoder_experiment) {
        return;
    }

    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        blog(LOG_INFO,
             "[obs-sync-replay] native encoder experiment autostart explicitly activated after "
             "frontend loading");
        native_encoder_experiment->Start();
    } else if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
        // OBS emits this before ClearSceneData(), audio/video teardown, and module
        // unloading. Stopping here keeps null_output's capture thread from
        // disconnecting global audio after its synchronization primitives are gone.
        blog(LOG_INFO,
             "[obs-sync-replay] native encoder experiment shutdown cleanup requested before "
             "OBS scene/audio teardown");
        native_encoder_experiment->Stop();
        if (native_encoder_experiment_frontend_callback_registered) {
            obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
            native_encoder_experiment_frontend_callback_registered = false;
        }
    }
}

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
    native_encoder_experiment_autostart_enabled = ExperimentalAutostartEnabled();
    if (native_encoder_experiment_autostart_enabled) {
        obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
        native_encoder_experiment_frontend_callback_registered = true;
        blog(LOG_INFO, "[obs-sync-replay] native encoder experiment autostart enabled; waiting for "
                       "frontend loading");
    } else {
        blog(LOG_INFO, "[obs-sync-replay] native encoder experiment disabled; set "
                       "OBS_SYNC_REPLAY_EXPERIMENT_AUTOSTART=1 for explicit research activation");
    }
    master_frame_coordinator = std::make_unique<obs_sync_replay::MasterFrameCoordinator>(
        [](const obs_sync_replay::MasterFrame& frame) {
            native_encoder_experiment->ObserveMasterFrame(frame);
        });
    master_frame_coordinator->Start();
    return true;
}

void obs_module_unload(void) {
    if (native_encoder_experiment_frontend_callback_registered) {
        obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
        native_encoder_experiment_frontend_callback_registered = false;
    }
    native_encoder_experiment_autostart_enabled = false;
    master_frame_coordinator.reset();
    native_encoder_experiment.reset();
    blog(LOG_INFO, "[obs-sync-replay] plugin unloaded");
}
