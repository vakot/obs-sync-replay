#pragma once

#include <cstring>
#include <functional>
#include <string>

namespace obs_sync_replay {

struct CaptureControlsLabels final {
    std::string start_recording;
    std::string stop_recording;
    std::string stopping_recording;
    std::string start_replay_buffer;
    std::string stop_replay_buffer;
    std::string stopping_replay_buffer;
    std::string save_replay;

    std::string starting_recording;
    std::string starting_replay_buffer;
    std::string saving_replay;
    std::string recording_unavailable;
    std::string replay_buffer_unavailable;
    std::string action_unavailable;
    std::string recording_tooltip;
    std::string replay_tooltip;
};

using CaptureControlsLocaleLookup = std::function<const char *(const char *)>;

// Resolve OBS frontend labels and plugin-owned transition/failure labels. The
// callbacks make the fallback behavior testable without requiring a running
// OBS frontend.
inline CaptureControlsLabels ResolveCaptureControlsLabels(const CaptureControlsLocaleLookup &frontend_lookup,
                                                          const CaptureControlsLocaleLookup &plugin_lookup) {
    const auto plugin = [&plugin_lookup](const char *key, const char *fallback) {
        if (plugin_lookup) {
            if (const char *value = plugin_lookup(key); value && *value && std::strcmp(value, key) != 0) {
                return std::string(value);
            }
        }
        return std::string(fallback);
    };
    const std::string action_unavailable =
        plugin("UI.Capture.ActionUnavailable", "Capture action unavailable");
    const auto frontend = [&frontend_lookup, &action_unavailable](const char *key) {
        if (frontend_lookup) {
            if (const char *value = frontend_lookup(key); value && *value) {
                return std::string(value);
            }
        }
        return action_unavailable;
    };

    CaptureControlsLabels labels;
    labels.start_recording = frontend("Basic.Main.StartRecording");
    labels.stop_recording = frontend("Basic.Main.StopRecording");
    labels.stopping_recording = frontend("Basic.Main.StoppingRecording");
    labels.start_replay_buffer = frontend("Basic.Main.StartReplayBuffer");
    labels.stop_replay_buffer = frontend("Basic.Main.StopReplayBuffer");
    labels.stopping_replay_buffer = frontend("Basic.Main.StoppingReplayBuffer");
    labels.save_replay = frontend("Basic.Main.SaveReplay");
    labels.starting_recording = plugin("UI.Capture.StartingRecording", "Starting capture...");
    labels.starting_replay_buffer = plugin("UI.Capture.StartingReplayBuffer", "Starting replay buffer...");
    labels.saving_replay = plugin("UI.Capture.SavingReplay", "Saving replay...");
    labels.recording_unavailable = plugin("UI.Capture.RecordingUnavailable", "Recording unavailable");
    labels.replay_buffer_unavailable =
        plugin("UI.Capture.ReplayBufferUnavailable", "Replay buffer unavailable");
    labels.action_unavailable = action_unavailable;
    labels.recording_tooltip = plugin(
        "Controls.PluginOwned.RecordingTooltip",
        "Recording is handled by OBS Sync Replay plugin");
    labels.replay_tooltip = plugin(
        "Controls.PluginOwned.ReplayTooltip",
        "Replay Buffer is handled by OBS Sync Replay plugin");
    return labels;
}

// Resolve labels from the running OBS frontend and this plugin's translation
// domain.
CaptureControlsLabels ResolveCaptureControlsLabels();

} // namespace obs_sync_replay
