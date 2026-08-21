#include <obs-module.h>

#include "bootstrap/deterministic-test-environment.hpp"
#include "experiment/stock-encoder-timeline-probe.hpp"
#include "rendering/synchronized-scene-renderer.hpp"
#include "timeline/master-frame-coordinator.hpp"

#include <memory>

namespace {

std::unique_ptr<obs_sync_replay::MasterFrameCoordinator> master_frame_coordinator;
std::unique_ptr<obs_sync_replay::SynchronizedSceneRenderer> synchronized_scene_renderer;
std::unique_ptr<obs_sync_replay::DeterministicTestEnvironment> deterministic_test_environment;
std::unique_ptr<obs_sync_replay::StockEncoderTimelineProbe> stock_encoder_timeline_probe;
bool bootstrap_registered = false;

void OnFrontendEvent(enum obs_frontend_event event, void *) {
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        return;
    }

    obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
    bootstrap_registered = false;

    deterministic_test_environment = std::make_unique<obs_sync_replay::DeterministicTestEnvironment>();
    if (!deterministic_test_environment->Setup()) {
        deterministic_test_environment.reset();
        blog(LOG_ERROR, "[obs-sync-replay] research bootstrap failed; synchronization experiment is disabled");
        return;
    }

    synchronized_scene_renderer = std::make_unique<obs_sync_replay::SynchronizedSceneRenderer>(
        obs_sync_replay::kResearchSceneAName, obs_sync_replay::kResearchSceneBName);
    master_frame_coordinator = std::make_unique<obs_sync_replay::MasterFrameCoordinator>(
        [](const obs_sync_replay::MasterFrame &frame) { synchronized_scene_renderer->Render(frame); });
    master_frame_coordinator->Start();
    stock_encoder_timeline_probe = std::make_unique<obs_sync_replay::StockEncoderTimelineProbe>(
        obs_sync_replay::kResearchSceneAName, obs_sync_replay::kResearchSceneBName);
    stock_encoder_timeline_probe->Start();
    blog(LOG_INFO, "[obs-sync-replay] research bootstrap ready; coordinator started after clean environment setup");
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
    return true;
}

void obs_module_post_load(void) {
    // OBS activates the scene collection before FINISHED_LOADING. The public
    // frontend callback also keeps scene creation on OBS's frontend thread.
    bootstrap_registered = true;
    obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
    blog(LOG_INFO, "[sync-bootstrap] scheduled for frontend finished-loading after scene-collection activation");
}

void obs_module_unload(void) {
    if (bootstrap_registered) {
        obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
        bootstrap_registered = false;
    }
    stock_encoder_timeline_probe.reset();
    master_frame_coordinator.reset();
    synchronized_scene_renderer.reset();
    deterministic_test_environment.reset();
    blog(LOG_INFO, "[obs-sync-replay] plugin unloaded");
}
