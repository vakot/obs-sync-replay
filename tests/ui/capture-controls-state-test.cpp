#include "ui/capture-controls-state.hpp"

#include <cstdlib>
#include <iostream>

namespace {

using namespace obs_sync_replay;

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void TestIdlePresentation() {
    const CaptureControlsPresentation presentation = MakeCaptureControlsPresentation(
        CaptureInfrastructureState::Idle, RecordingConsumerState::Off, ReplayConsumerState::Off, false, false);
    Require(presentation.recording.state == CaptureControlVisualState::Inactive, "idle recording state");
    Require(presentation.recording.text == "Start Recording" && presentation.recording.enabled,
            "idle recording button");
    Require(presentation.replay.text == "Start Replay Buffer" && presentation.replay.enabled,
            "idle replay button");
    Require(!presentation.save_replay_enabled, "save must be disabled while replay is inactive");
}

void TestTransitionsAndSave() {
    const CaptureControlsPresentation starting = MakeCaptureControlsPresentation(
        CaptureInfrastructureState::Active, RecordingConsumerState::Starting, ReplayConsumerState::Starting, false, false);
    Require(starting.recording.state == CaptureControlVisualState::Starting && !starting.recording.enabled,
            "recording start transition must disable the button");
    Require(starting.replay.state == CaptureControlVisualState::Starting && !starting.replay.enabled,
            "replay start transition must disable the button");

    const CaptureControlsPresentation active = MakeCaptureControlsPresentation(
        CaptureInfrastructureState::Active, RecordingConsumerState::Running, ReplayConsumerState::Running, false, false);
    Require(active.recording.text == "Stop Recording" && active.recording.enabled, "active recording button");
    Require(active.replay.text == "Stop Replay Buffer" && active.replay.enabled, "active replay button");
    Require(active.save_replay_enabled, "save must be enabled while replay is active");

    const CaptureControlsPresentation saving = MakeCaptureControlsPresentation(
        CaptureInfrastructureState::Active, RecordingConsumerState::Running, ReplayConsumerState::Saving, false, false);
    Require(saving.replay.state == CaptureControlVisualState::Saving && !saving.replay.enabled,
            "saving must disable replay toggle");
    Require(!saving.save_replay_enabled, "saving must prevent duplicate save");
}

void TestFailurePresentation() {
    const CaptureControlsPresentation failed = MakeCaptureControlsPresentation(
        CaptureInfrastructureState::Failed, RecordingConsumerState::Off, ReplayConsumerState::Off, false, false);
    Require(failed.recording.state == CaptureControlVisualState::Failed && !failed.recording.enabled,
            "infrastructure failure must disable recording");
    Require(failed.replay.state == CaptureControlVisualState::Failed && !failed.replay.enabled,
            "infrastructure failure must disable replay");

    const CaptureControlsPresentation command_failed = MakeCaptureControlsPresentation(
        CaptureInfrastructureState::Idle, RecordingConsumerState::Off, ReplayConsumerState::Off, false, true);
    Require(command_failed.replay.state == CaptureControlVisualState::Failed && !command_failed.replay.enabled,
            "replay command failure must be visible in replay control state");
}

} // namespace

int main() {
    TestIdlePresentation();
    TestTransitionsAndSave();
    TestFailurePresentation();
    return EXIT_SUCCESS;
}
