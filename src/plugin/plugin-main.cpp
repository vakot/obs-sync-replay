#include <obs-frontend-api.h>
#include <obs-hotkey.h>
#include <obs-module.h>

#include <QtWidgets/QDockWidget>
#include <QtWidgets/QWidget>

#include "bootstrap/deterministic-test-environment.hpp"
#include "control/plugin-capture-runtime.hpp"
#include "ui/plugin-control-dock.hpp"

#include <atomic>
#include <memory>

namespace {

constexpr char kControlDockId[] = "obsSyncReplayControlDock";
constexpr char kControlDockTitle[] = "Synchronized Capture";

std::unique_ptr<obs_sync_replay::DeterministicTestEnvironment> deterministic_test_environment;
std::unique_ptr<obs_sync_replay::PluginCaptureRuntime> capture_runtime;
obs_sync_replay::PluginControlDock* control_dock = nullptr;
QDockWidget* control_dock_container = nullptr;
std::atomic<bool> shutdown_requested{false};
bool frontend_registered = false;
obs_hotkey_pair_id recording_hotkeys = OBS_INVALID_HOTKEY_PAIR_ID;
obs_hotkey_pair_id replay_hotkeys = OBS_INVALID_HOTKEY_PAIR_ID;
obs_hotkey_id save_replay_hotkey = OBS_INVALID_HOTKEY_ID;

void LogCommand(const char* action, const obs_sync_replay::ControlCommandResult& result) {
    blog(result.ok() ? LOG_INFO : LOG_ERROR, "[plugin-control] action=%s status=%s reason=%s", action,
         obs_sync_replay::ControlCommandStatusName(result.status), result.reason.c_str());
}

bool OnStartRecordingHotkey(void* data, obs_hotkey_pair_id, obs_hotkey_t*, bool pressed) {
    auto* runtime = static_cast<obs_sync_replay::PluginCaptureRuntime*>(data);
    if (!runtime || !pressed || runtime->recording_state() != obs_sync_replay::RecordingConsumerState::Off) {
        return false;
    }
    LogCommand("hotkey-recording-start", runtime->StartRecording());
    return true;
}

bool OnStopRecordingHotkey(void* data, obs_hotkey_pair_id, obs_hotkey_t*, bool pressed) {
    auto* runtime = static_cast<obs_sync_replay::PluginCaptureRuntime*>(data);
    if (!runtime || !pressed || runtime->recording_state() != obs_sync_replay::RecordingConsumerState::Running) {
        return false;
    }
    LogCommand("hotkey-recording-stop", runtime->StopRecording());
    return true;
}

bool OnStartReplayHotkey(void* data, obs_hotkey_pair_id, obs_hotkey_t*, bool pressed) {
    auto* runtime = static_cast<obs_sync_replay::PluginCaptureRuntime*>(data);
    if (!runtime || !pressed || runtime->replay_state() != obs_sync_replay::ReplayConsumerState::Off) {
        return false;
    }
    LogCommand("hotkey-replay-start", runtime->StartReplay());
    return true;
}

bool OnStopReplayHotkey(void* data, obs_hotkey_pair_id, obs_hotkey_t*, bool pressed) {
    auto* runtime = static_cast<obs_sync_replay::PluginCaptureRuntime*>(data);
    if (!runtime || !pressed || runtime->replay_state() != obs_sync_replay::ReplayConsumerState::Running) {
        return false;
    }
    LogCommand("hotkey-replay-stop", runtime->StopReplay());
    return true;
}

void OnSaveReplayHotkey(void* data, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    auto* runtime = static_cast<obs_sync_replay::PluginCaptureRuntime*>(data);
    if (!runtime || !pressed || runtime->replay_state() != obs_sync_replay::ReplayConsumerState::Running) {
        return;
    }
    LogCommand("hotkey-replay-save", runtime->SaveReplay());
}

void RegisterPluginHotkeys() {
    if (!capture_runtime) {
        return;
    }
    recording_hotkeys = obs_hotkey_pair_register_frontend(
        "OBS Sync Replay.StartRecording", "Start Synchronized Recording", "OBS Sync Replay.StopRecording",
        "Stop Synchronized Recording", OnStartRecordingHotkey, OnStopRecordingHotkey, capture_runtime.get(),
        capture_runtime.get());
    replay_hotkeys = obs_hotkey_pair_register_frontend(
        "OBS Sync Replay.StartReplay", "Start Synchronized Replay", "OBS Sync Replay.StopReplay",
        "Stop Synchronized Replay", OnStartReplayHotkey, OnStopReplayHotkey, capture_runtime.get(),
        capture_runtime.get());
    save_replay_hotkey = obs_hotkey_register_frontend("OBS Sync Replay.SaveReplay", "Save Synchronized Replay",
                                                      OnSaveReplayHotkey, capture_runtime.get());
    blog(LOG_INFO, "[plugin-hotkey] registered recording=%llu replay=%llu save=%llu",
         static_cast<unsigned long long>(recording_hotkeys), static_cast<unsigned long long>(replay_hotkeys),
         static_cast<unsigned long long>(save_replay_hotkey));
}

void UnregisterPluginHotkeys() {
    if (recording_hotkeys != OBS_INVALID_HOTKEY_PAIR_ID) {
        obs_hotkey_pair_unregister(recording_hotkeys);
        recording_hotkeys = OBS_INVALID_HOTKEY_PAIR_ID;
    }
    if (replay_hotkeys != OBS_INVALID_HOTKEY_PAIR_ID) {
        obs_hotkey_pair_unregister(replay_hotkeys);
        replay_hotkeys = OBS_INVALID_HOTKEY_PAIR_ID;
    }
    if (save_replay_hotkey != OBS_INVALID_HOTKEY_ID) {
        obs_hotkey_unregister(save_replay_hotkey);
        save_replay_hotkey = OBS_INVALID_HOTKEY_ID;
    }
}

void StopPluginOwnedRuntime(const char* boundary) {
    if (!capture_runtime && !control_dock && !deterministic_test_environment) {
        return;
    }
    if (shutdown_requested.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (control_dock) {
        control_dock->DisableControls();
    }
    UnregisterPluginHotkeys();
    if (capture_runtime) {
        capture_runtime->Stop();
    }
    if (control_dock_container) {
        obs_frontend_remove_dock(kControlDockId);
        delete control_dock_container;
        control_dock_container = nullptr;
    }
    control_dock = nullptr;
    capture_runtime.reset();
    if (deterministic_test_environment) {
        deterministic_test_environment.reset();
    }
    blog(LOG_INFO, "[sync-shutdown] boundary=%s complete plugin-owned runtime stopped", boundary);
}

void OnFrontendEvent(enum obs_frontend_event event, void*) {
    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP || event == OBS_FRONTEND_EVENT_EXIT) {
        StopPluginOwnedRuntime(event == OBS_FRONTEND_EVENT_EXIT ? "frontend-exit" : "scene-collection-cleanup");
        if (event == OBS_FRONTEND_EVENT_EXIT && frontend_registered) {
            obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
            frontend_registered = false;
        }
        return;
    }
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING || shutdown_requested.load(std::memory_order_acquire)) {
        return;
    }

    deterministic_test_environment = std::make_unique<obs_sync_replay::DeterministicTestEnvironment>();
    if (!deterministic_test_environment->Setup()) {
        deterministic_test_environment.reset();
        blog(LOG_ERROR, "[obs-sync-replay] plugin-owned runtime disabled: research scenes unavailable");
        return;
    }

    capture_runtime = std::make_unique<obs_sync_replay::PluginCaptureRuntime>(
        obs_sync_replay::kResearchSceneAName, obs_sync_replay::kResearchSceneBName);
    if (!capture_runtime->Initialize()) {
        blog(LOG_ERROR, "[obs-sync-replay] plugin-owned runtime initialization failed");
        capture_runtime.reset();
        return;
    }

    auto* dock = new QDockWidget(QString::fromUtf8(kControlDockTitle),
                                 static_cast<QWidget*>(obs_frontend_get_main_window()));
    dock->setObjectName(QString::fromUtf8(kControlDockId));
    auto* widget = new obs_sync_replay::PluginControlDock(*capture_runtime, dock);
    dock->setWidget(widget);
    if (!obs_frontend_add_custom_qdock(kControlDockId, dock)) {
        delete dock;
        blog(LOG_ERROR, "[plugin-ui] dock-registration-failed id=%s", kControlDockId);
        capture_runtime->Stop();
        capture_runtime.reset();
        return;
    }
    dock->setVisible(true);
    control_dock = widget;
    control_dock_container = dock;
    RegisterPluginHotkeys();
    blog(LOG_INFO, "[obs-sync-replay] plugin-owned controls ready recording=off replay=off active_encoders=0");
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
    shutdown_requested.store(false, std::memory_order_release);
    blog(LOG_INFO, "[obs-sync-replay] plugin loaded (version %s); plugin-owned capture idle", OBS_SYNC_REPLAY_VERSION);
    return true;
}

void obs_module_post_load(void) {
    frontend_registered = true;
    obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
    blog(LOG_INFO, "[sync-bootstrap] waiting for frontend finished-loading; no capture will start automatically");
}

void obs_module_unload(void) {
    blog(LOG_INFO, "[sync-shutdown] module-unload begin");
    StopPluginOwnedRuntime("module-unload");
    if (frontend_registered) {
        obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
        frontend_registered = false;
    }
    blog(LOG_INFO, "[obs-sync-replay] plugin unloaded");
}
