#include "ui/capture-controls-state.hpp"

#include <cstdlib>
#include <iostream>

namespace {

using namespace obs_sync_replay;

CaptureControlsLabels LocalizedLabels() {
    CaptureControlsLabels labels;
    labels.start_recording = "Localized start recording";
    labels.stop_recording = "Localized stop recording";
    labels.stopping_recording = "Localized stopping recording...";
    labels.start_replay_buffer = "Localized start replay buffer";
    labels.stop_replay_buffer = "Localized stop replay buffer";
    labels.stopping_replay_buffer = "Localized stopping replay buffer...";
    labels.save_replay = "Localized save replay";
    labels.starting_recording = "Localized starting capture...";
    labels.starting_replay_buffer = "Localized starting replay buffer...";
    labels.saving_replay = "Localized saving replay...";
    labels.recording_unavailable = "Localized recording unavailable";
    labels.replay_buffer_unavailable = "Localized replay buffer unavailable";
    labels.action_unavailable = "Localized capture action unavailable";
    return labels;
}

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void TestIdlePresentation() {
    const CaptureControlsLabels labels = LocalizedLabels();
    const CaptureControlsPresentation presentation = MakeCaptureControlsPresentation(
        CaptureInfrastructureState::Idle, RecordingConsumerState::Off, ReplayConsumerState::Off, false, false, labels);
    Require(presentation.recording.state == CaptureControlVisualState::Inactive, "idle recording state");
    Require(presentation.recording.text == labels.start_recording && presentation.recording.enabled,
            "idle recording button");
    Require(presentation.replay.text == labels.start_replay_buffer && presentation.replay.enabled,
            "idle replay button");
    Require(presentation.save_replay_text == labels.save_replay, "save label must come from localization");
    Require(!presentation.save_replay_enabled, "save must be disabled while replay is inactive");
}

void TestTransitionsAndSave() {
    const CaptureControlsLabels labels = LocalizedLabels();
    const CaptureControlsPresentation starting = MakeCaptureControlsPresentation(
        CaptureInfrastructureState::Active, RecordingConsumerState::Starting, ReplayConsumerState::Starting, false, false,
        labels);
    Require(starting.recording.state == CaptureControlVisualState::Starting && !starting.recording.enabled,
            "recording start transition must disable the button");
    Require(starting.recording.text == labels.starting_recording, "recording transition label must be localized");
    Require(starting.replay.state == CaptureControlVisualState::Starting && !starting.replay.enabled,
            "replay start transition must disable the button");
    Require(starting.replay.text == labels.starting_replay_buffer, "replay transition label must be localized");

    const CaptureControlsPresentation active = MakeCaptureControlsPresentation(
        CaptureInfrastructureState::Active, RecordingConsumerState::Running, ReplayConsumerState::Running, false, false,
        labels);
    Require(active.recording.text == labels.stop_recording && active.recording.enabled, "active recording button");
    Require(active.replay.text == labels.stop_replay_buffer && active.replay.enabled, "active replay button");
    Require(active.save_replay_enabled, "save must be enabled while replay is active");

    const CaptureControlsPresentation saving = MakeCaptureControlsPresentation(
        CaptureInfrastructureState::Active, RecordingConsumerState::Running, ReplayConsumerState::Saving, false, false,
        labels);
    Require(saving.replay.state == CaptureControlVisualState::Saving && !saving.replay.enabled,
            "saving must disable replay toggle");
    Require(saving.replay.text == labels.saving_replay, "save transition label must be localized");
    Require(!saving.save_replay_enabled, "saving must prevent duplicate save");
}

void TestFailurePresentation() {
    const CaptureControlsLabels labels = LocalizedLabels();
    const CaptureControlsPresentation failed = MakeCaptureControlsPresentation(
        CaptureInfrastructureState::Failed, RecordingConsumerState::Off, ReplayConsumerState::Off, false, false, labels);
    Require(failed.recording.state == CaptureControlVisualState::Failed && !failed.recording.enabled,
            "infrastructure failure must disable recording");
    Require(failed.recording.text == labels.recording_unavailable, "recording failure must use plugin localization");
    Require(failed.replay.state == CaptureControlVisualState::Failed && !failed.replay.enabled,
            "infrastructure failure must disable replay");
    Require(failed.replay.text == labels.replay_buffer_unavailable, "replay failure must use plugin localization");

    const CaptureControlsPresentation command_failed = MakeCaptureControlsPresentation(
        CaptureInfrastructureState::Idle, RecordingConsumerState::Off, ReplayConsumerState::Off, false, true, labels);
    Require(command_failed.replay.state == CaptureControlVisualState::Failed && !command_failed.replay.enabled,
            "replay command failure must be visible in replay control state");
}

void TestLookupFallbacks() {
    const CaptureControlsLabels labels = ResolveCaptureControlsLabels(
        [](const char *) { return static_cast<const char *>(nullptr); },
        [](const char *) { return static_cast<const char *>(nullptr); });
    Require(!labels.start_recording.empty() && !labels.save_replay.empty(),
            "frontend lookup failure must leave native labels usable");
    Require(labels.start_recording == labels.action_unavailable,
            "frontend lookup failure must use the safe plugin fallback");
    Require(!labels.recording_unavailable.empty() && labels.recording_unavailable != "UI.Capture.RecordingUnavailable",
            "plugin lookup failure must not expose a raw locale key");
}

void TestObsAndPluginLookupKeys() {
    const CaptureControlsLabels labels = ResolveCaptureControlsLabels(
        [](const char *key) {
            if (std::string(key) == "Basic.Main.StartRecording") {
                return "OBS localized start recording";
            }
            if (std::string(key) == "Basic.Main.StopRecording") {
                return "OBS localized stop recording";
            }
            if (std::string(key) == "Basic.Main.StoppingRecording") {
                return "OBS localized stopping recording";
            }
            if (std::string(key) == "Basic.Main.StartReplayBuffer") {
                return "OBS localized start replay";
            }
            if (std::string(key) == "Basic.Main.StopReplayBuffer") {
                return "OBS localized stop replay";
            }
            if (std::string(key) == "Basic.Main.StoppingReplayBuffer") {
                return "OBS localized stopping replay";
            }
            if (std::string(key) == "Basic.Main.SaveReplay") {
                return "OBS localized save replay";
            }
            return static_cast<const char *>(nullptr);
        },
        [](const char *key) {
            if (std::string(key) == "UI.Capture.StartingRecording") {
                return "Plugin localized starting recording";
            }
            if (std::string(key) == "UI.Capture.StartingReplayBuffer") {
                return "Plugin localized starting replay";
            }
            if (std::string(key) == "UI.Capture.SavingReplay") {
                return "Plugin localized saving replay";
            }
            return static_cast<const char *>(nullptr);
        });
    Require(labels.start_recording == "OBS localized start recording" &&
                labels.stop_recording == "OBS localized stop recording" &&
                labels.save_replay == "OBS localized save replay",
            "native labels must use OBS frontend lookup keys");
    Require(labels.starting_recording == "Plugin localized starting recording" &&
                labels.starting_replay_buffer == "Plugin localized starting replay" &&
                labels.saving_replay == "Plugin localized saving replay",
            "plugin-only transitions must use plugin lookup keys");
}

} // namespace

int main() {
    TestIdlePresentation();
    TestTransitionsAndSave();
    TestFailurePresentation();
    TestLookupFallbacks();
    TestObsAndPluginLookupKeys();
    return EXIT_SUCCESS;
}
