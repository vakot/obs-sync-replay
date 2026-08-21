#pragma once

#include "control/capture-control.hpp"
#include "ui/capture-controls-localization.hpp"

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

constexpr bool CaptureControlUsesNativeActiveStyle(const CaptureControlVisualState state) noexcept {
    return state == CaptureControlVisualState::Active || state == CaptureControlVisualState::Saving ||
           state == CaptureControlVisualState::Stopping;
}

struct CaptureButtonPresentation final {
    CaptureControlVisualState state = CaptureControlVisualState::Inactive;
    std::string text;
    bool enabled = false;
    bool visible = true;
};

struct CaptureControlsPresentation final {
    CaptureButtonPresentation recording;
    CaptureButtonPresentation replay;
    std::string save_replay_text;
    bool save_replay_visible = false;
    bool save_replay_enabled = false;
};

inline CaptureControlsPresentation MakeCaptureControlsPresentation(
    const CaptureInfrastructureState infrastructure, const RecordingConsumerState recording,
    const ReplayConsumerState replay, const bool replay_available, const bool recording_failed,
    const bool replay_failed, const CaptureControlsLabels &labels) {
    CaptureControlsPresentation presentation;
    presentation.save_replay_text = labels.save_replay;

    if (infrastructure == CaptureInfrastructureState::Failed || recording_failed) {
        presentation.recording = {CaptureControlVisualState::Failed, labels.recording_unavailable, false, true};
    } else {
        switch (recording) {
        case RecordingConsumerState::Off:
            presentation.recording = {CaptureControlVisualState::Inactive, labels.start_recording, true, true};
            break;
        case RecordingConsumerState::Starting:
            presentation.recording = {CaptureControlVisualState::Starting, labels.starting_recording, false, true};
            break;
        case RecordingConsumerState::Running:
            presentation.recording = {CaptureControlVisualState::Active, labels.stop_recording, true, true};
            break;
        case RecordingConsumerState::Stopping:
            presentation.recording = {CaptureControlVisualState::Stopping, labels.stopping_recording, false, true};
            break;
        }
    }

    if (!replay_available) {
        presentation.replay = {CaptureControlVisualState::Inactive, labels.replay_buffer_unavailable, false, false};
    } else if (infrastructure == CaptureInfrastructureState::Failed || replay_failed) {
        presentation.replay = {CaptureControlVisualState::Failed, labels.replay_buffer_unavailable, false, true};
    } else {
        switch (replay) {
        case ReplayConsumerState::Off:
            presentation.replay = {CaptureControlVisualState::Inactive, labels.start_replay_buffer, true, true};
            break;
        case ReplayConsumerState::Starting:
            presentation.replay = {CaptureControlVisualState::Starting, labels.starting_replay_buffer, false, true};
            break;
        case ReplayConsumerState::Running:
            presentation.replay = {CaptureControlVisualState::Active, labels.stop_replay_buffer, true, true};
            presentation.save_replay_visible = true;
            presentation.save_replay_enabled = true;
            break;
        case ReplayConsumerState::Saving:
            presentation.replay = {CaptureControlVisualState::Saving, labels.stop_replay_buffer, false, true};
            presentation.save_replay_visible = true;
            break;
        case ReplayConsumerState::Stopping:
            presentation.replay = {CaptureControlVisualState::Stopping, labels.stopping_replay_buffer, false, true};
            presentation.save_replay_visible = true;
            break;
        }
    }

    return presentation;
}

} // namespace obs_sync_replay
