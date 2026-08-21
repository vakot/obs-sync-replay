#include "control/capture-control.hpp"

#include <algorithm>
#include <utility>

namespace obs_sync_replay {

namespace {

bool IsRecordingActive(const RecordingConsumerState state) {
    return state != RecordingConsumerState::Off;
}

bool IsReplayActive(const ReplayConsumerState state) {
    return state != ReplayConsumerState::Off;
}

} // namespace

CaptureControlEngine::CaptureControlEngine(CaptureConfiguration configuration, SynchronizedCaptureSession& capture,
                                           EncoderController& encoders, DiagnosticSink diagnostic_sink,
                                           const uint32_t replay_save_delay_ms)
    : configuration_(std::move(configuration)), capture_(capture), encoders_(encoders),
      diagnostic_sink_(std::move(diagnostic_sink)), replay_save_delay_ms_(replay_save_delay_ms) {}

CaptureControlEngine::~CaptureControlEngine() {
    Shutdown();
}

bool CaptureControlEngine::Initialize() {
    if (initialized_) {
        return true;
    }
    initialized_ = RegisterStreams();
    if (!initialized_) {
        capture_state_ = CaptureInfrastructureState::Failed;
    }
    return initialized_;
}

ControlCommandResult CaptureControlEngine::StartRecording(std::vector<std::filesystem::path> paths) {
    if (!initialized_) {
        return Invalid("not-initialized");
    }
    if (recording_state_ == RecordingConsumerState::Running ||
        recording_state_ == RecordingConsumerState::Starting) {
        return {ControlCommandStatus::NoOp, "recording-already-active"};
    }
    if (recording_state_ != RecordingConsumerState::Off || paths.size() != SelectedCaptureIds(CaptureConsumer::Recording).size() ||
        paths.empty()) {
        return Invalid("recording-path-count-or-state");
    }

    recording_state_ = RecordingConsumerState::Starting;
    recording_result_.reset();
    recording_consumer_ = std::make_unique<SynchronizedRecordingConsumer>(
        SelectedPacketConfigs(CaptureConsumer::Recording), std::move(paths), SelectedCaptureIds(CaptureConsumer::Recording));
    if (!recording_consumer_->Start() || !capture_.Subscribe(recording_consumer_.get())) {
        recording_consumer_.reset();
        recording_state_ = RecordingConsumerState::Off;
        return {ControlCommandStatus::Failed, "recording-consumer-start"};
    }

    // Recording-only capture must not retain a replay ring. Replay retention is
    // enabled only by the independent Replay consumer state.
    capture_.SetReplayRetentionEnabled(IsReplayActive(replay_state_));
    if (!EnsureCaptureActive() || !ReconcileEncoderDemand()) {
        capture_.Unsubscribe(recording_consumer_.get());
        recording_consumer_->Stop();
        recording_consumer_.reset();
        recording_state_ = RecordingConsumerState::Off;
        StopCaptureIfUnused();
        return {ControlCommandStatus::Failed, "recording-infrastructure-start"};
    }
    recording_state_ = RecordingConsumerState::Running;
    return {ControlCommandStatus::Succeeded, "recording-started"};
}

ControlCommandResult CaptureControlEngine::StopRecording() {
    if (recording_state_ == RecordingConsumerState::Off) {
        return {ControlCommandStatus::NoOp, "recording-already-off"};
    }
    if (!recording_consumer_) {
        recording_state_ = RecordingConsumerState::Off;
        return {ControlCommandStatus::Failed, "recording-consumer-missing"};
    }

    recording_state_ = RecordingConsumerState::Stopping;
    capture_.Unsubscribe(recording_consumer_.get());
    recording_consumer_->Stop();
    recording_result_ = recording_consumer_->result();
    recording_consumer_.reset();
    recording_state_ = RecordingConsumerState::Off;
    ReleaseUndemandedEncoders();
    StopCaptureIfUnused();
    return {ControlCommandStatus::Succeeded, "recording-stopped"};
}

ControlCommandResult CaptureControlEngine::StartReplay() {
    if (!initialized_) {
        return Invalid("not-initialized");
    }
    if (replay_state_ == ReplayConsumerState::Running || replay_state_ == ReplayConsumerState::Saving) {
        return {ControlCommandStatus::NoOp, "replay-already-active"};
    }
    if (replay_state_ != ReplayConsumerState::Off || SelectedCaptureIds(CaptureConsumer::Replay).empty()) {
        return Invalid("replay-state-or-no-streams");
    }

    replay_state_ = ReplayConsumerState::Running;
    replay_result_.reset();
    capture_.SetReplayRetentionEnabled(true);
    if (!EnsureCaptureActive() || !ReconcileEncoderDemand()) {
        replay_state_ = ReplayConsumerState::Off;
        capture_.SetReplayRetentionEnabled(false);
        StopCaptureIfUnused();
        return {ControlCommandStatus::Failed, "replay-infrastructure-start"};
    }
    return {ControlCommandStatus::Succeeded, "replay-started"};
}

ControlCommandResult CaptureControlEngine::StopReplay() {
    if (replay_state_ == ReplayConsumerState::Off) {
        return {ControlCommandStatus::NoOp, "replay-already-off"};
    }

    replay_state_ = ReplayConsumerState::Stopping;
    WaitForReplaySave();
    replay_state_ = ReplayConsumerState::Off;
    capture_.SetReplayRetentionEnabled(false);
    ReleaseUndemandedEncoders();
    StopCaptureIfUnused();
    return {ControlCommandStatus::Succeeded, "replay-stopped"};
}

ControlCommandResult CaptureControlEngine::SaveReplay(std::vector<std::filesystem::path> paths) {
    if (replay_state_ == ReplayConsumerState::Off) {
        return Invalid("replay-off");
    }
    if (replay_state_ == ReplayConsumerState::Saving) {
        return Invalid("replay-save-active");
    }
    if (paths.size() != SelectedCaptureIds(CaptureConsumer::Replay).size() || paths.empty()) {
        return Invalid("replay-path-count");
    }
    if (!replay_consumer_) {
        replay_consumer_ = std::make_unique<SynchronizedReplayConsumer>(capture_, replay_save_delay_ms_);
    }
    if (!replay_consumer_->RequestSave(std::move(paths), configuration_.replay_duration_ns,
                                       SelectedCaptureIds(CaptureConsumer::Replay))) {
        return {ControlCommandStatus::Failed, "replay-save-rejected"};
    }
    replay_state_ = ReplayConsumerState::Saving;
    return {ControlCommandStatus::Succeeded, "replay-save-started"};
}

void CaptureControlEngine::WaitForReplaySave() noexcept {
    if (!replay_consumer_) {
        return;
    }
    replay_consumer_->Wait();
    if (replay_state_ == ReplayConsumerState::Saving) {
        replay_result_ = replay_consumer_->last_result();
        replay_state_ = ReplayConsumerState::Running;
    }
}

void CaptureControlEngine::Shutdown() noexcept {
    if (!initialized_ && capture_state_ == CaptureInfrastructureState::Idle) {
        return;
    }
    StopReplay();
    StopRecording();
    capture_.Stop();
    capture_.SetReplayRetentionEnabled(false);
    for (size_t index = 0; index < configuration_.streams.size(); ++index) {
        const ConfiguredStream& stream = configuration_.streams[index];
        const auto capture_id = CaptureIdFor(stream.identity);
        if (capture_id && encoders_.IsActive(*capture_id)) {
            encoders_.Release(*capture_id, stream);
            Emit(EncoderLifecycleEvent::Released, *capture_id);
        }
    }
    capture_state_ = CaptureInfrastructureState::Idle;
}

const CaptureConfiguration& CaptureControlEngine::configuration() const noexcept {
    return configuration_;
}

CaptureInfrastructureState CaptureControlEngine::capture_state() const noexcept {
    return capture_state_;
}

RecordingConsumerState CaptureControlEngine::recording_state() const noexcept {
    return recording_state_;
}

ReplayConsumerState CaptureControlEngine::replay_state() const noexcept {
    return replay_state_;
}

size_t CaptureControlEngine::active_encoder_count() const noexcept {
    return encoders_.active_count();
}

uint64_t CaptureControlEngine::capture_epoch() const noexcept {
    return capture_epoch_;
}

std::optional<SynchronizedRecordingConsumerResult> CaptureControlEngine::recording_result() const {
    return recording_result_;
}

std::optional<ReplaySaveResult> CaptureControlEngine::replay_result() const {
    return replay_result_;
}

std::optional<CaptureStreamId> CaptureControlEngine::CaptureIdFor(const StreamIdentity identity) const noexcept {
    CaptureStreamId capture_id = 0;
    for (const ConfiguredStream& stream : configuration_.streams) {
        if (stream.mode == StreamParticipationMode::Disabled) {
            continue;
        }
        if (stream.identity == identity) {
            return capture_id;
        }
        ++capture_id;
    }
    return std::nullopt;
}

bool CaptureControlEngine::RegisterStreams() {
    CaptureStreamId capture_id = 0;
    for (const ConfiguredStream& stream : configuration_.streams) {
        if (stream.mode == StreamParticipationMode::Disabled ||
            !capture_.RegisterStream(capture_id++, stream.name, stream.packet_config)) {
            if (stream.mode != StreamParticipationMode::Disabled) {
                return false;
            }
        }
    }
    return capture_id > 0;
}

bool CaptureControlEngine::EnsureCaptureActive() {
    if (capture_state_ == CaptureInfrastructureState::Active && capture_.running()) {
        return true;
    }
    if (capture_state_ == CaptureInfrastructureState::Failed || !capture_.Start()) {
        capture_state_ = CaptureInfrastructureState::Failed;
        return false;
    }
    ++capture_epoch_;
    capture_state_ = CaptureInfrastructureState::Active;
    return true;
}

bool CaptureControlEngine::ReconcileEncoderDemand() {
    const bool recording_active = IsRecordingActive(recording_state_);
    const bool replay_active = IsReplayActive(replay_state_);
    for (const ConfiguredStream& stream : configuration_.streams) {
        const auto capture_id = CaptureIdFor(stream.identity);
        if (!capture_id || stream.mode == StreamParticipationMode::Disabled) {
            continue;
        }
        if (!encoders_.IsActive(*capture_id)) {
            if (!encoders_.EnsureCreated(*capture_id, stream)) {
                return false;
            }
        }
    }
    // OBS encoder groups must be complete before the first member starts. The
    // create pass above makes a consumer handoff add only newly demanded
    // encoders; the activation pass never restarts already active members.
    for (const ConfiguredStream& stream : configuration_.streams) {
        const auto capture_id = CaptureIdFor(stream.identity);
        if (!capture_id || !StreamNeedsEncoder(stream.mode, recording_active, replay_active)) {
            continue;
        }
        if (!encoders_.IsActive(*capture_id)) {
            if (!encoders_.Activate(*capture_id, stream)) {
                return false;
            }
            Emit(EncoderLifecycleEvent::Activated, *capture_id);
        } else {
            Emit(EncoderLifecycleEvent::Retained, *capture_id);
        }
    }
    return true;
}

void CaptureControlEngine::ReleaseUndemandedEncoders() noexcept {
    const bool recording_active = IsRecordingActive(recording_state_);
    const bool replay_active = IsReplayActive(replay_state_);
    for (const ConfiguredStream& stream : configuration_.streams) {
        const auto capture_id = CaptureIdFor(stream.identity);
        if (!capture_id || StreamNeedsEncoder(stream.mode, recording_active, replay_active) ||
            !encoders_.IsActive(*capture_id)) {
            continue;
        }
        encoders_.Release(*capture_id, stream);
        Emit(EncoderLifecycleEvent::Released, *capture_id);
    }
}

void CaptureControlEngine::StopCaptureIfUnused() noexcept {
    if (IsRecordingActive(recording_state_) || IsReplayActive(replay_state_)) {
        return;
    }
    capture_state_ = CaptureInfrastructureState::Stopping;
    capture_.Stop();
    capture_.SetReplayRetentionEnabled(false);
    ReleaseUndemandedEncoders();
    capture_state_ = CaptureInfrastructureState::Idle;
}

std::vector<CaptureStreamId> CaptureControlEngine::SelectedCaptureIds(const CaptureConsumer consumer) const {
    std::vector<CaptureStreamId> ids;
    CaptureStreamId capture_id = 0;
    for (const ConfiguredStream& stream : configuration_.streams) {
        if (stream.mode == StreamParticipationMode::Disabled) {
            continue;
        }
        if (StreamParticipates(stream.mode, consumer)) {
            ids.push_back(capture_id);
        }
        ++capture_id;
    }
    return ids;
}

std::vector<PacketStreamConfig> CaptureControlEngine::SelectedPacketConfigs(const CaptureConsumer consumer) const {
    std::vector<PacketStreamConfig> configs;
    for (const ConfiguredStream& stream : configuration_.streams) {
        if (StreamParticipates(stream.mode, consumer)) {
            configs.push_back(stream.packet_config);
        }
    }
    return configs;
}

void CaptureControlEngine::Emit(const EncoderLifecycleEvent event, const CaptureStreamId stream_id) const {
    if (diagnostic_sink_) {
        diagnostic_sink_({event, stream_id, encoders_.active_count()});
    }
}

ControlCommandResult CaptureControlEngine::Invalid(const char* const reason) const {
    return {ControlCommandStatus::InvalidState, reason};
}

const char* ControlCommandStatusName(const ControlCommandStatus status) noexcept {
    switch (status) {
    case ControlCommandStatus::Succeeded:
        return "succeeded";
    case ControlCommandStatus::NoOp:
        return "no-op";
    case ControlCommandStatus::InvalidState:
        return "invalid-state";
    case ControlCommandStatus::Failed:
        return "failed";
    }
    return "unknown";
}

} // namespace obs_sync_replay
