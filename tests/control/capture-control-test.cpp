#include "control/capture-control.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace obs_sync_replay;

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

PacketStreamConfig StreamConfig() {
    PacketStreamConfig config;
    config.width = 1920;
    config.height = 1080;
    config.timebase_num = 1;
    config.timebase_den = 60000;
    config.extra_data = {0x01, 0x42, 0xc0, 0x2a, 0xff, 0xe1, 0x00, 0x1c, 0x67, 0x42, 0xc0,
                         0x2a, 0xda, 0x01, 0xe0, 0x08, 0x9f, 0x97, 0x01, 0x6a, 0x02, 0x02,
                         0x02, 0x80, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x3c, 0x47,
                         0x8c, 0x19, 0x50, 0x01, 0x00, 0x04, 0x68, 0xce, 0x3c, 0x80};
    return config;
}

EncodedPacket Packet(const uint64_t source_cts, const bool keyframe = false) {
    EncodedPacket packet;
    packet.source_cts = source_cts;
    packet.pts = static_cast<int64_t>(source_cts / 1'000'000);
    packet.dts = packet.pts;
    packet.timebase_num = 1;
    packet.timebase_den = 60000;
    packet.keyframe = keyframe;
    packet.payload = {0x00, 0x00, 0x01, static_cast<uint8_t>(keyframe ? 0x65 : 0x41)};
    return packet;
}

CaptureConfiguration Configuration(const StreamParticipationMode master, const StreamParticipationMode scene_a,
                                   const StreamParticipationMode scene_b) {
    CaptureConfiguration configuration;
    configuration.replay.enabled = true;
    configuration.replay.target_duration_ns = 1;
    configuration.replay.memory_budget_bytes = 1024 * 1024;
    configuration.streams = {{StreamIdentity::Master(), "master", master, StreamConfig()},
                             {StreamIdentity::Scene("scene-a-uuid"), "scene_a", scene_a, StreamConfig()},
                             {StreamIdentity::Scene("scene-b-uuid"), "scene_b", scene_b, StreamConfig()}};
    return configuration;
}

void RemovePaths(const std::vector<std::filesystem::path>& paths) {
    for (const auto& path : paths) {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
}

std::vector<std::filesystem::path> Paths(const char* const stem, const size_t count) {
    std::vector<std::filesystem::path> paths;
    for (size_t index = 0; index < count; ++index) {
        paths.push_back(std::filesystem::temp_directory_path() /
                        (std::string(stem) + "-" + std::to_string(index) + ".mkv"));
    }
    return paths;
}

class FakeEncoderController final : public EncoderController {
  public:
    bool EnsureCreated(const CaptureStreamId stream_id, const ConfiguredStream&) override {
        created_.insert(stream_id);
        return true;
    }

    bool Activate(const CaptureStreamId stream_id, const ConfiguredStream&) override {
        return created_.find(stream_id) != created_.end() && active_.insert(stream_id).second;
    }

    void Release(const CaptureStreamId stream_id, const ConfiguredStream&) noexcept override {
        active_.erase(stream_id);
    }

    bool IsActive(const CaptureStreamId stream_id) const noexcept override {
        return active_.find(stream_id) != active_.end();
    }

    size_t active_count() const noexcept override {
        return active_.size();
    }

    std::set<CaptureStreamId> created_;
    std::set<CaptureStreamId> active_;
};

void TestModeSemantics() {
    Require(!StreamParticipates(StreamParticipationMode::Disabled, CaptureConsumer::Recording),
            "disabled must not record");
    Require(!StreamParticipates(StreamParticipationMode::Disabled, CaptureConsumer::Replay),
            "disabled must not replay");
    Require(StreamParticipates(StreamParticipationMode::Recording, CaptureConsumer::Recording),
            "recording mode must record");
    Require(!StreamParticipates(StreamParticipationMode::Recording, CaptureConsumer::Replay),
            "recording mode must not replay");
    Require(StreamNeedsEncoder(StreamParticipationMode::Both, true, true), "both needs one encoder");
    Require(!StreamNeedsEncoder(StreamParticipationMode::Replay, true, false), "replay-only is idle without replay");
    Require(StreamNeedsEncoder(StreamParticipationMode::Replay, true, true), "replay-only needs replay encoder");
}

void TestHandoffAndEncoderOwnership() {
    CaptureConfiguration configuration = Configuration(StreamParticipationMode::Both, StreamParticipationMode::Both,
                                                        StreamParticipationMode::Both);
    SynchronizedCaptureConfig capture_config;
    capture_config.ring_capacity_bytes = 1024 * 1024;
    SynchronizedCaptureSession capture(capture_config);
    FakeEncoderController encoders;
    std::vector<EncoderLifecycleEvent> events;
    CaptureControlEngine engine(configuration, capture, encoders,
                                [&events](const EncoderLifecycleDiagnostic& diagnostic) {
                                    events.push_back(diagnostic.event);
                                });
    Require(engine.Initialize(), "initialize all-both engine");

    Require(engine.StartReplay().ok(), "start replay first");
    Require(engine.active_encoder_count() == 3, "replay-only must use three encoders");
    for (CaptureStreamId stream = 0; stream < 3; ++stream) {
        Require(capture.Ingest(stream, Packet(100, true)), "replay packet reaches shared capture");
        Require(capture.Ingest(stream, Packet(110)), "replay history advances on shared capture");
    }
    const auto replay_only_paths = Paths("control-replay-only", 3);
    Require(engine.SaveReplay(replay_only_paths).ok(), "replay-only save starts after common history");
    engine.WaitForReplaySave();
    if (!engine.replay_result() || !engine.replay_result()->success) {
        std::cerr << "replay-only save error: "
                  << (engine.replay_result() ? engine.replay_result()->error : "missing-result") << '\n';
        Require(false, "replay-only save must finalize all synchronized streams");
    }
    RemovePaths(replay_only_paths);
    const size_t replay_epoch = engine.capture_epoch();
    Require(engine.StartRecording(Paths("control-handoff-recording", 3)).ok(), "attach recording to replay");
    Require(engine.active_encoder_count() == 3, "recording handoff must retain three encoders");
    for (CaptureStreamId stream = 0; stream < 3; ++stream) {
        Require(capture.Ingest(stream, Packet(200, true)), "recording packet reaches shared capture");
        Require(capture.Ingest(stream, Packet(210)), "recording common prefix advances");
    }
    Require(engine.StopRecording().ok(), "stop recording while replay continues");
    Require(engine.recording_result() && engine.recording_result()->success,
            "recording consumer must finalize synchronized MKVs");
    Require(engine.active_encoder_count() == 3 && engine.replay_state() == ReplayConsumerState::Running,
            "replay must continue after recording stop");
    Require(engine.StopReplay().ok(), "stop replay");
    Require(engine.active_encoder_count() == 0 && engine.capture_state() == CaptureInfrastructureState::Idle,
            "all consumers off must release encoders and capture");
    Require(engine.StartRecording(Paths("control-inverse-recording", 3)).ok(), "start recording first");
    Require(engine.StartReplay().ok(), "attach replay to recording");
    Require(engine.active_encoder_count() == 3, "inverse handoff must use three encoders");
    Require(engine.StopReplay().ok() && engine.active_encoder_count() == 3,
            "recording must survive replay stop");
    Require(engine.StopRecording().ok() && engine.capture_epoch() == replay_epoch + 1,
            "total idle must create a fresh capture epoch");
    Require(!events.empty(), "encoder lifecycle diagnostics must be emitted");
}

void TestMixedModesAndInvalidCommands() {
    CaptureConfiguration configuration = Configuration(StreamParticipationMode::Both,
                                                        StreamParticipationMode::Recording,
                                                        StreamParticipationMode::Replay);
    SynchronizedCaptureSession capture;
    FakeEncoderController encoders;
    CaptureControlEngine engine(configuration, capture, encoders);
    Require(engine.Initialize(), "initialize mixed engine");
    Require(engine.SaveReplay(Paths("invalid-save", 1)).status == ControlCommandStatus::InvalidState,
            "save while replay is off must be invalid");
    Require(engine.StartRecording(Paths("mixed-recording", 2)).ok(), "start mixed recording");
    Require(engine.active_encoder_count() == 2, "mixed recording must use master and scene A");
    Require(capture.Ingest(0, Packet(100, true)) && capture.Ingest(1, Packet(100, true)),
            "mixed recording packets must be accepted");
    Require(capture.metrics().retained_bytes == 0, "recording-only capture must not retain replay packets");
    Require(engine.StartReplay().ok(), "start mixed replay");
    Require(engine.active_encoder_count() == 3, "mixed replay must add scene B only");
    Require(engine.SaveReplay(Paths("mixed-empty-replay", 2)).reason == "replay-save-rejected:insufficient-history",
            "replay rejection must preserve the specific history reason");
    Require(engine.StartReplay().status == ControlCommandStatus::NoOp, "start replay must be idempotent");
    Require(engine.StopReplay().ok() && engine.active_encoder_count() == 2,
            "mixed replay stop must retain recording encoders");
    Require(engine.StopRecording().ok() && engine.active_encoder_count() == 0,
            "mixed recording stop must release remaining encoders");

    CaptureConfiguration disabled = Configuration(StreamParticipationMode::Both, StreamParticipationMode::Disabled,
                                                  StreamParticipationMode::Replay);
    SynchronizedCaptureSession disabled_capture;
    FakeEncoderController disabled_encoders;
    CaptureControlEngine disabled_engine(disabled, disabled_capture, disabled_encoders);
    Require(disabled_engine.Initialize(), "initialize disabled engine");
    Require(disabled_engine.StartRecording(Paths("disabled-recording", 1)).ok(), "start disabled test recording");
    Require(disabled_engine.active_encoder_count() == 1, "disabled stream must never start an encoder");
    Require(disabled_engine.StopRecording().ok(), "stop disabled test recording");
}

void TestReplayConfigurationLifecycle() {
    CaptureConfiguration configuration = Configuration(StreamParticipationMode::Both, StreamParticipationMode::Both,
                                                       StreamParticipationMode::Both);
    SynchronizedCaptureSession capture;
    FakeEncoderController encoders;
    CaptureControlEngine engine(configuration, capture, encoders);
    Require(engine.Initialize(), "initialize replay configuration engine");
    Require(engine.StartReplay().ok(), "configured replay must start");
    Require(engine.StartRecording(Paths("config-recording", 3)).ok(), "recording must overlap replay");

    ReplayConfiguration disabled = configuration.replay;
    disabled.enabled = false;
    Require(engine.ApplyReplayConfiguration(disabled).ok(), "disabling replay configuration must succeed");
    Require(!engine.replay_available() && engine.replay_state() == ReplayConsumerState::Off,
            "disabling replay must stop replay and reject availability");
    Require(engine.recording_state() == RecordingConsumerState::Running && engine.active_encoder_count() == 3,
            "disabling replay must not stop recording or its encoders");
    Require(engine.StartReplay().status == ControlCommandStatus::InvalidState,
            "disabled replay must reject start");
    Require(engine.SaveReplay(Paths("disabled-save", 3)).status == ControlCommandStatus::InvalidState,
            "disabled replay must reject save");

    disabled.enabled = true;
    Require(engine.ApplyReplayConfiguration(disabled).ok(), "reenabling replay configuration must succeed");
    Require(engine.replay_state() == ReplayConsumerState::Off, "reenabling replay must not auto-start it");
    Require(engine.StartReplay().ok(), "reenabled replay must start on explicit command");
    Require(engine.StopReplay().ok() && engine.StopRecording().ok(), "configured replay lifecycle must stop cleanly");
}

} // namespace

int main() {
    TestModeSemantics();
    TestHandoffAndEncoderOwnership();
    TestMixedModesAndInvalidCommands();
    TestReplayConfigurationLifecycle();
    return EXIT_SUCCESS;
}
