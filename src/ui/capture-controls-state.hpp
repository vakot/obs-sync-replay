#pragma once

#include "control/capture-control.hpp"

#include <string>

namespace obs_sync_replay {

enum class CaptureControlVisualState : uint8_t {
    Inactive,
    Starting,
    Active,
    Saving,
    Stopping,
    Failed,
};

struct CaptureButtonPresentation final {
    CaptureControlVisualState state = CaptureControlVisualState::Inactive;
    std::string text;
    bool enabled = false;
    bool visible = true;
};

struct CaptureControlsPresentation final {
    CaptureButtonPresentation recording;
    CaptureButtonPresentation replay;
    bool save_replay_enabled = false;
};

inline CaptureControlsPresentation MakeCaptureControlsPresentation(
    const CaptureInfrastructureState infrastructure, const RecordingConsumerState recording,
    const ReplayConsumerState replay, const bool recording_failed, const bool replay_failed) {
    CaptureControlsPresentation presentation;

    if (infrastructure == CaptureInfrastructureState::Failed || recording_failed) {
        presentation.recording = {CaptureControlVisualState::Failed, "Recording unavailable", false, true};
    } else {
        switch (recording) {
        case RecordingConsumerState::Off:
            presentation.recording = {CaptureControlVisualState::Inactive, "Start Recording", true, true};
            break;
        case RecordingConsumerState::Starting:
            presentation.recording = {CaptureControlVisualState::Starting, "Starting Recording...", false, true};
            break;
        case RecordingConsumerState::Running:
            presentation.recording = {CaptureControlVisualState::Active, "Stop Recording", true, true};
            break;
        case RecordingConsumerState::Stopping:
            presentation.recording = {CaptureControlVisualState::Stopping, "Stopping Recording...", false, true};
            break;
        }
    }

    if (infrastructure == CaptureInfrastructureState::Failed || replay_failed) {
        presentation.replay = {CaptureControlVisualState::Failed, "Replay Buffer unavailable", false, true};
    } else {
        switch (replay) {
        case ReplayConsumerState::Off:
            presentation.replay = {CaptureControlVisualState::Inactive, "Start Replay Buffer", true, true};
            break;
        case ReplayConsumerState::Starting:
            presentation.replay = {CaptureControlVisualState::Starting, "Starting Replay Buffer...", false, true};
            break;
        case ReplayConsumerState::Running:
            presentation.replay = {CaptureControlVisualState::Active, "Stop Replay Buffer", true, true};
            presentation.save_replay_enabled = true;
            break;
        case ReplayConsumerState::Saving:
            presentation.replay = {CaptureControlVisualState::Saving, "Saving Replay...", false, true};
            break;
        case ReplayConsumerState::Stopping:
            presentation.replay = {CaptureControlVisualState::Stopping, "Stopping Replay Buffer...", false, true};
            break;
        }
    }

    return presentation;
}

} // namespace obs_sync_replay
