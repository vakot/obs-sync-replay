#pragma once

#include "control/capture-configuration.hpp"
#include "recording/synchronized-recording-consumer.hpp"
#include "replay/synchronized-replay-consumer.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace obs_sync_replay {

enum class CaptureInfrastructureState : uint8_t {
    Idle,
    Active,
    Stopping,
    Failed,
};

enum class RecordingConsumerState : uint8_t {
    Off,
    Starting,
    Running,
    Stopping,
};

enum class ReplayConsumerState : uint8_t {
    Off,
    Running,
    Saving,
    Stopping,
};

enum class ControlCommandStatus : uint8_t {
    Succeeded,
    NoOp,
    InvalidState,
    Failed,
};

struct ControlCommandResult final {
    ControlCommandStatus status = ControlCommandStatus::Failed;
    std::string reason;

    bool ok() const noexcept {
        return status == ControlCommandStatus::Succeeded || status == ControlCommandStatus::NoOp;
    }
};

enum class EncoderLifecycleEvent : uint8_t {
    Created,
    Activated,
    Retained,
    Released,
};

struct EncoderLifecycleDiagnostic final {
    EncoderLifecycleEvent event = EncoderLifecycleEvent::Created;
    CaptureStreamId stream_id = 0;
    size_t active_encoder_count = 0;
};

class EncoderController {
  public:
    virtual ~EncoderController() = default;
    virtual bool EnsureCreated(CaptureStreamId stream_id, const ConfiguredStream& stream) = 0;
    virtual bool Activate(CaptureStreamId stream_id, const ConfiguredStream& stream) = 0;
    virtual void Release(CaptureStreamId stream_id, const ConfiguredStream& stream) noexcept = 0;
    virtual bool IsActive(CaptureStreamId stream_id) const noexcept = 0;
    virtual size_t active_count() const noexcept = 0;
};

class CaptureControlEngine final {
  public:
    using DiagnosticSink = std::function<void(const EncoderLifecycleDiagnostic&)>;

    CaptureControlEngine(CaptureConfiguration configuration, SynchronizedCaptureSession& capture,
                         EncoderController& encoders, DiagnosticSink diagnostic_sink = {},
                         uint32_t replay_save_delay_ms = 0);
    ~CaptureControlEngine();

    CaptureControlEngine(const CaptureControlEngine&) = delete;
    CaptureControlEngine& operator=(const CaptureControlEngine&) = delete;

    bool Initialize();
    ControlCommandResult StartRecording(std::vector<std::filesystem::path> paths);
    ControlCommandResult StopRecording();
    ControlCommandResult StartReplay();
    ControlCommandResult StopReplay();
    ControlCommandResult SaveReplay(std::vector<std::filesystem::path> paths);
    void WaitForReplaySave() noexcept;
    void Shutdown() noexcept;

    const CaptureConfiguration& configuration() const noexcept;
    CaptureInfrastructureState capture_state() const noexcept;
    RecordingConsumerState recording_state() const noexcept;
    ReplayConsumerState replay_state() const noexcept;
    size_t active_encoder_count() const noexcept;
    uint64_t capture_epoch() const noexcept;
    std::optional<SynchronizedRecordingConsumerResult> recording_result() const;
    std::optional<ReplaySaveResult> replay_result() const;
    std::optional<CaptureStreamId> CaptureIdFor(StreamIdentity identity) const noexcept;

  private:
    bool RegisterStreams();
    bool EnsureCaptureActive();
    bool ReconcileEncoderDemand();
    void ReleaseUndemandedEncoders() noexcept;
    void StopCaptureIfUnused() noexcept;
    std::vector<CaptureStreamId> SelectedCaptureIds(CaptureConsumer consumer) const;
    std::vector<PacketStreamConfig> SelectedPacketConfigs(CaptureConsumer consumer) const;
    void Emit(EncoderLifecycleEvent event, CaptureStreamId stream_id) const;
    ControlCommandResult Invalid(const char* reason) const;

    CaptureConfiguration configuration_;
    SynchronizedCaptureSession& capture_;
    EncoderController& encoders_;
    DiagnosticSink diagnostic_sink_;
    uint32_t replay_save_delay_ms_ = 0;
    std::unique_ptr<SynchronizedRecordingConsumer> recording_consumer_;
    std::unique_ptr<SynchronizedReplayConsumer> replay_consumer_;
    std::optional<SynchronizedRecordingConsumerResult> recording_result_;
    std::optional<ReplaySaveResult> replay_result_;
    CaptureInfrastructureState capture_state_ = CaptureInfrastructureState::Idle;
    RecordingConsumerState recording_state_ = RecordingConsumerState::Off;
    ReplayConsumerState replay_state_ = ReplayConsumerState::Off;
    bool initialized_ = false;
    uint64_t capture_epoch_ = 0;
};

const char* ControlCommandStatusName(ControlCommandStatus status) noexcept;

} // namespace obs_sync_replay
