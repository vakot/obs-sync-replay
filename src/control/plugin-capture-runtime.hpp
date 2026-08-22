#pragma once

#include "config/replay-configuration.hpp"
#include "control/capture-control.hpp"
#include "topology/scene-topology.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace obs_sync_replay {

struct DiscoveredObsScene;

class PluginCaptureRuntime final {
public:
    PluginCaptureRuntime();
    ~PluginCaptureRuntime();

    PluginCaptureRuntime(const PluginCaptureRuntime &) = delete;
    PluginCaptureRuntime &operator=(const PluginCaptureRuntime &) = delete;

    bool Initialize();
    ControlCommandResult ToggleRecording();
    ControlCommandResult StartRecording();
    ControlCommandResult StopRecording();
    ControlCommandResult ToggleReplay();
    ControlCommandResult StartReplay();
    ControlCommandResult SaveReplay();
    ControlCommandResult StopReplay();
    ControlCommandResult ApplyReplayConfiguration(ReplayConfiguration configuration);
    ControlCommandResult RefreshReplayConfiguration();
    ControlCommandResult RefreshSceneTopology();
    void PollReplaySave() noexcept;
    void Stop();

    bool initialized() const noexcept;
    CaptureInfrastructureState capture_state() const noexcept;
    RecordingConsumerState recording_state() const noexcept;
    ReplayConsumerState replay_state() const noexcept;
    size_t active_encoder_count() const noexcept;
    bool replay_available() const noexcept;

private:
    struct State;
    struct ControlState;

    std::vector<std::filesystem::path> OutputPaths(CaptureConsumer consumer, const char *stem);
    ControlCommandResult Failed(const char *reason) const;
    bool BuildControlState();
    bool InstallSceneTargets(std::vector<struct DiscoveredObsScene> discovered);
    void ResetSceneTargets() noexcept;
    void FinishCaptureEpochIfIdle();
    void LogTopology(const char* event) const;

    ReplayConfiguration replay_configuration_;
    SceneTopologyModel topology_model_;
    std::unique_ptr<State> state_;
    std::unique_ptr<ControlState> control_state_;
    mutable std::mutex mutex_;
    uint64_t recording_number_ = 0;
    uint64_t replay_number_ = 0;
    uint64_t replay_save_generation_ = 0;
    uint64_t replay_result_logged_generation_ = 0;
};

} // namespace obs_sync_replay
